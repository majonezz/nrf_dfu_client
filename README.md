### Nordic Legacy DFU Tool written in C

Based on: <br>
https://github.com/infsoft-locaware/nrfdfu<br>
https://github.com/recrof/nrf_dfu_py<br>
https://github.com/michaelrsweet/zipc<br>
https://github.com/rpz80/json<br>

Depends on: *libzlib* and *libbluetooth*.

My goal was to run it on OpenWrt based router at minimal requirements.
There is an example Makefile for OpenWrt named Makefile.openwrt, so you can build a package for your router.
One of the use cases can be upgrading a firmware on remote located NRF based Meshcore repeater.
You will need a Bluetooth adapter with BLE capability.
Tested with 0a5c:21e8 Broadcom Corp BCM20702A0 Bluetooth adapter.<br>

Example usage: 
- Upload NRF update package to the router, for example via Luci's System->Software->Upload Package button. Upload it, but don't install of course.
- Open ssh connection to the router and bring bluetooth adapter on via ```hciconfig hci0 up ```
- Scan BLE devices by typing ```hcitool lescan```. You will get a list of mac addresses. Check if your Meshcore repeater is on the list.
- Do the update: ```nrf_dfu -a XX:XX:XX:XX:XX:XX -t random /tmp/upload.ipk```

Build: ```make```

```
root@OpenWrt:~# hcitool lescan
LE Scan ...
D7:26:0E:21:2C:1B SENSECAP_SOLAR_OTA
root@OpenWrt:~# nrf_dfu -a D7:26:0E:21:2C:1B -t random /tmp/upload.ipk
433648 from 433648 bytes sent (100%)
Firmware sent.
Validate ok.
root@OpenWrt:~#
```
