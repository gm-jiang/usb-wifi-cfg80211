#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/gpio.h>
#include <net/cfg80211.h> /* wiphy and probably everything that would required for FullMAC driver */

#include "esp_cfg80211.h"
#include "link_glue.h"
#include "msg.h"
#include "log.h"

#if !defined(LINUX_VERSION_CODE)
#include <linux/version.h>
#endif

//#define LINUX_VERSION_CODE   KERNEL_VERSION(4, 4, 0)

//#define NO_USB_SIMULATOR 1

MODULE_LICENSE("GPL");
MODULE_AUTHOR("esp1986");
MODULE_DESCRIPTION("esp1986 diy usb 802.11 wifi solution four esp32 s2/s3 chip");
MODULE_VERSION("0.1");

int esp_scan_done_evt(struct esp_cfg80211_adapter *apt, uint8_t *resp, bool aborted);

static int process_tx_packet(struct sk_buff *skb);
int esp_send_packet(struct esp_cfg80211_adapter *adapter, struct sk_buff *skb);

struct esp_ndev_priv_context {
    struct esp_cfg80211_adapter *apt;
    struct wireless_dev wdev;
};

#define NDEV_NAME "esp_wlan%d"

/* helper function that will retrieve main context from "priv" data of the network device */
static struct esp_ndev_priv_context *ndev_get_esp_context(struct net_device *ndev)
{
    return (struct esp_ndev_priv_context *)netdev_priv(ndev);
}

int link_tx_packet(struct esp_cfg80211_adapter *adapter, struct sk_buff *skb)
{
    if (!adapter) {
        return -EINVAL;
    }
    if (adapter->ndev && !netif_queue_stopped((const struct net_device *)adapter->ndev)) {
        netif_stop_queue(adapter->ndev);
    }

    link_tx_msg(adapter->bottom_obj, skb->data, skb->len);
    dev_kfree_skb(skb);

    return 0;
}

static int process_tx_packet(struct sk_buff *skb)
{
    struct esp_cfg80211_skb_cb *cb = NULL;
    struct esp_cfg80211_adapter *apt = NULL;
    int ret = 0;
    u16 len = 0;

    cb = (struct esp_cfg80211_skb_cb *)skb->cb;
    apt = cb->priv;

    if (!apt) {
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    if (netif_queue_stopped((const struct net_device *)apt->ndev)) {
        return NETDEV_TX_BUSY;
    }

    len = skb->len;
    if (1) {
        ret = link_tx_packet(apt, skb);

        if (ret) {
            apt->stats.tx_errors++;
        } else {
            apt->stats.tx_packets++;
            apt->stats.tx_bytes += skb->len;
        }
    } else {
        dev_kfree_skb_any(skb);
        apt->stats.tx_dropped++;
    }

    return 0;
}

static void process_rx_packet(struct esp_cfg80211_adapter *apt, struct sk_buff *skb)
{
    if (!skb) {
        return;
    }

    skb->dev = apt->ndev;
    skb->protocol = eth_type_trans(skb, apt->ndev);
    skb->ip_summed = CHECKSUM_NONE;

    /* Forward skb to kernel */
    netif_rx_ni(skb);

    apt->stats.rx_bytes += skb->len;
    apt->stats.rx_packets++;
}

static struct sk_buff *link_rx_packet(struct esp_cfg80211_adapter *apt)
{
    struct sk_buff *skb = NULL;

    if (!apt) {
        LOGE("%s: Invalid apt:%p\n", __func__, apt);
        return NULL;
    }

    skb = skb_dequeue(&(apt->rx_queue));

    return skb;
}

static void link_rx_work(struct work_struct *work)
{
    struct esp_cfg80211_adapter *apt = container_of(work, struct esp_cfg80211_adapter, if_rx_work);

    struct sk_buff *skb = NULL;

    if (!apt) {
        return;
    }

    skb = link_rx_packet(apt);

    if (!skb) {
        return;
    }

    process_rx_packet(apt, skb);

    return;
}

static void deinit_adapter(struct esp_cfg80211_adapter *apt)
{
    /* Flush workqueues */
    if (apt->if_rx_workqueue) {
        flush_workqueue(apt->if_rx_workqueue);
        destroy_workqueue(apt->if_rx_workqueue);
    }
}

static netdev_tx_t esp_ndo_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct esp_cfg80211_adapter *apt = ndev_get_esp_context(dev)->apt;
    struct esp_cfg80211_skb_cb *cb = NULL;

    if (!apt) {
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    if (!skb->len || (skb->len > ETH_FRAME_LEN)) {
        apt->stats.tx_dropped++;
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }
    cb = (struct esp_cfg80211_skb_cb *)skb->cb;
    cb->priv = apt;

    return process_tx_packet(skb);
}

static int esp_ndo_open(struct net_device *ndev)
{
    netif_start_queue(ndev);
    return 0;
}

static int esp_ndo_stop(struct net_device *ndev)
{
    netif_stop_queue(ndev);
    return 0;
}

static struct net_device_stats *esp_ndo_get_stats(struct net_device *ndev)
{
    struct esp_cfg80211_adapter *apt = ndev_get_esp_context(ndev)->apt;
    return &apt->stats;
}

/* Structure of functions for network devices.
 * It should have at least ndo_start_xmit functions that called for packet to be sent. */
static struct net_device_ops esp_ndev_ops = {
    .ndo_open = esp_ndo_open,
    .ndo_stop = esp_ndo_stop,
    .ndo_start_xmit = esp_ndo_start_xmit,
    .ndo_get_stats = esp_ndo_get_stats,

};

static struct esp_cfg80211_adapter *init_adapter(void *bottom_obj)
{
    struct wireless_dev *wdev;
    struct esp_cfg80211_adapter *apt = kzalloc(sizeof(struct esp_cfg80211_adapter), GFP_KERNEL);
    u8 mac_address[6] = { 0x7c, 0xdf, 0xa1, 0x93, 0x9f, 0x96 };

    {
#ifdef NO_USB_SIMULATOR

#else

        cmd_resp64_t cmd;
        cmd.cmd_op = OUT_BAND_CMD_GET_MAC;
        if (link_issue_cmd_resp(bottom_obj, (uint8_t *)&cmd, sizeof(cmd), (uint8_t *)&cmd, sizeof(cmd)) >= 0) {
            memcpy(mac_address, cmd.buf, 6);
        } else {
            LOGW("%s can't get mac addr\n", __FUNCTION__);
        }

#endif
    }

    if (esp_cfg80211_wiphy_init(apt) < 0) {
        goto error_wiphy;
    }
/* allocate network device context. */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0))
    apt->ndev = alloc_netdev(sizeof(struct esp_ndev_priv_context), NDEV_NAME, ether_setup);
#else
    apt->ndev = alloc_netdev(sizeof(struct esp_ndev_priv_context), NDEV_NAME, 0, ether_setup);
#endif
    if (apt->ndev == NULL) {
        LOGE("%s alloc netdev NG\n", __FUNCTION__);
        goto error_alloc_ndev;
    }
    /* fill private data of network context.*/
    ndev_get_esp_context(apt->ndev)->apt = apt;
    wdev = &ndev_get_esp_context(apt->ndev)->wdev;
    /* fill wireless_dev context.
     * wireless_dev with net_device can be represented as inherited class of single net_device. */
    wdev->wiphy = apt->wiphy;
    wdev->netdev = apt->ndev;
    wdev->iftype = NL80211_IFTYPE_STATION;
    apt->ndev->ieee80211_ptr = wdev;

    wiphy_ext_feature_set(apt->wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
    /* set device object for net_device */
    /* SET_NETDEV_DEV(ret->ndev, wiphy_dev(ret->wiphy)); */

    /* set network device hooks. It should implement ndo_start_xmit() at least. */
    apt->ndev->netdev_ops = &esp_ndev_ops;

    /* Add here proper net_device initialization. */
    // register network device. If everything ok, there should be new network device:
    // $ ip link show

    ether_addr_copy(apt->ndev->dev_addr, mac_address);
    if (register_netdev(apt->ndev)) {
        LOGE("%s register netdev NG\n", __FUNCTION__);
        goto error_ndev_register;
    }

    /* Prepare interface RX work */
    apt->if_rx_workqueue = create_workqueue("ESP_WIFI_RX_WORK_QUEUE");

    if (!apt->if_rx_workqueue) {
        deinit_adapter(apt);
        goto error_ndev_register;
    }

    INIT_WORK(&apt->if_rx_work, link_rx_work);

    return apt;
error_ndev_register:
    free_netdev(apt->ndev);
error_alloc_ndev:
    wiphy_unregister(apt->wiphy);
error_wiphy:
    return NULL;
}

void upper_layer_queue_wakeup(void *uobj)
{
    struct esp_cfg80211_adapter *adapter = uobj;

    if (adapter && netif_queue_stopped((const struct net_device *)adapter->ndev)) {
        netif_wake_queue(adapter->ndev);
    }
}

struct esp_cfg80211_adapter *gp_apt = NULL;

void *esp_cfg80211_init(void *bottom_obj)
{
    int ret = 0;
    struct esp_cfg80211_adapter *apt = NULL;

    {
        /* Init adapter */
        apt = init_adapter(bottom_obj);
    }

    if (!apt) {
        return NULL;
    }

    skb_queue_head_init(&apt->rx_queue);

    if (ret != 0) {
        deinit_adapter(apt);
    }

    apt->bottom_obj = bottom_obj;

    gp_apt = apt;
    return apt;
}

void esp_cfg80211_exit(void)
{
    struct esp_cfg80211_adapter *apt = gp_apt;

    netif_stop_queue(apt->ndev); // stop queue first
    // then wait for a skb done, we doesn't use a sync way, just wait it done
    esp_scan_done_evt(apt, NULL, true);
    msleep(1000);
    //
    skb_queue_purge(&apt->rx_queue);
    deinit_adapter(apt);
    /* make sure that no work is queued */

    apt->bottom_obj = NULL;
    msg_queue_uninit(&apt->msg_mgr);
    cancel_work_sync(&apt->ws_connect);
    cancel_work_sync(&apt->ws_disconnect);
    cancel_work_sync(&apt->ws_scan);

    unregister_netdev(apt->ndev);
    free_netdev(apt->ndev);
    wiphy_unregister(apt->wiphy);
    wiphy_free(apt->wiphy);
}

int __init register_usb_link(void);
void __exit unregister_usb_link(void);

static int __init esp_usb_wifi_init(void)
{
#ifdef NO_USB_SIMULATOR
    esp_cfg80211_init(NULL);

#else
    return register_usb_link();
#endif
    return 0;
}

static void __exit esp_usb_wifi_exit(void)
{
#ifdef NO_USB_SIMULATOR
    esp_cfg80211_exit();
#else
    unregister_usb_link();
#endif
}

module_init(esp_usb_wifi_init);
module_exit(esp_usb_wifi_exit);
