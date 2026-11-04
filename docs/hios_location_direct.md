# HiOS 定位直连

定位页面不再使用 `services.url` 或电脑的 `8766` 端口。其它应用的服务器配置不受此修改影响。

- GPS：板载 GNSS/NMEA 快照提供 WGS84 坐标，不需要网络计算坐标。
- WiFi：NT26 `AT+ECWIFISCAN` 扫描后，以最多 32 个热点 MAC/RSSI 直接 POST 到 `https://api.beacondb.net/v1/geolocate`。
- 基站：NT26 `AT+ECBCINFO` 读取 LTE MCC/MNC/TAC/ECI/RSSI，直接请求 beaconDB。
- 网络定位设置 `considerIp=false`；不会用出口 IP 代替设备位置。请求不附带 IMEI、设备 ID 或 WiFi 名称。
- 地址：设备直接调用 Photon `/reverse`。地址服务不可用时仍保留定位坐标。
- 地图：设备下载 OSM PNG 瓦片，逐块解码并拼接 RGB565 地图，中心标记当前位置。地图失败不撤销坐标结果。

地图需要可写 SD 卡。`/sdcard/mapcache` 保存压缩瓦片至少七天，仅下载当前视图需要的瓦片，不预取；地图窗口保留 OpenStreetMap 署名。新图最大 672×672 RGB565，拼接时同时解码一个 256×256 瓦片。旧图在新图成功后替换。

错误消息区分 beaconDB、Photon 和 OSM，连接失败包含底层错误码；HTTP 失败显示状态码。beaconDB 的 404 也可能表示数据库未覆盖扫描到的热点/基站，并不等同于网络断开。公共服务可达性和数据覆盖需要在设备实际网络上确认。

源码：`apps/metalio/display/screen/gps_screen/gps_direct.inc`（网络适配和地图拼接）、`gps_screen.cxx`（扫描、请求和界面）。

验证：`python3 scripts/test_gps_direct.py` 检查 GPS、热点/基站载荷、无 IP 回退、无 IMEI、缺失坐标、网络错误、地址失败的部分成功及无效坐标。需要系统 C/C++ 编译器。设备交叉编译与完整链接记录在 `output/gps_direct`；这些检查不代替实机 GNSS、扫描覆盖、地图性能和网络验证。

服务资料：
- https://beacondb.net/
- https://github.com/komoot/photon/blob/master/docs/api-v1.md
- https://operations.osmfoundation.org/policies/tiles/
