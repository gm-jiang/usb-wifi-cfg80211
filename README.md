# Ubuntu18.04 无线网卡驱动 (FullMAC Mode)

## 1. 硬件

* PC with OS Ubuntu18.04
* ESP32-S2 Saola-1_v1.2

## 2. 系统

* Ubuntu18.04

  * 系统版本：`Linux ubuntu-optiplex-3050 5.4.0-113-generic #127~18.04.1-Ubuntu SMP Wed May 18 15:40:23 UTC 2022 x86_64 x86_64 x86_64 GNU/Linux`
  * GCC 版本：`gcc version 7.5.0 (Ubuntu 7.5.0-3ubuntu1~18.04)`
  * 软件包支持: `iperf`
  * 驱动编译支持：
  * Wi-Fi 配置管理工具：`wpa_supplicant`

* ESP32-S2 Saola-1_v1.2

	* ESP-IDF version
	* Toolchain: 

## 3.固件

* source code: 

## 4. 驱动

* 编译
* `modprobe cfg80211`
* `insmod xxx.ko`

## 5. 测试

* 修改 `/etc/wpasupplicant/wpa_supplicant.conf` 文件，添加如下内容：

```
network={
　　ssid="[网络ssid]"
　　psk="[密码]"
　　priority=1
}
```

* 连接

```
 sudo wpa_supplicant -i wlan0 -c /etc/wpa_supplicant.conf
```

* 查看

```apl
xwifi0: flags=-28605<UP,BROADCAST,RUNNING,MULTICAST,DYNAMIC>  mtu 1500
        inet 192.168.50.102  netmask 255.255.255.0  broadcast 192.168.50.255
        inet6 fe80::7edf:a1ff:fe00:39f6  prefixlen 64  scopeid 0x20<link>
        ether 7c:df:a1:00:39:f6  txqueuelen 1000  (Ethernet)
        RX packets 4133  bytes 582881 (569.2 KiB)
        RX errors 0  dropped 0  overruns 0  frame 0
        TX packets 13327  bytes 14515027 (13.8 MiB)
        TX errors 0  dropped 0 overruns 0  carrier 0  collisions 0
```

## 6. 遗留问题

* Run on openwrt 
