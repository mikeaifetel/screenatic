# Screenatic on Vega OS (Fire TV) — feasibility probe

Before porting Screenatic to Vega OS we need answers the docs don't give. `app/` + `native/` is a
small probe app that opens the same sockets a real receiver would and shows on screen what happens.

| Folder | What |
|---|---|
| `native/` | C++ TurboModule: TCP listeners on 7000 (AirPlay) / 49595 (DLNA) / 7100 (video ingest), mDNS responder advertising `_airplay._tcp`, SSDP MediaRenderer announcer |
| `app/` | React Native for Vega UI: live status, peers that reached us, MSE video latency test |
| `scripts/stream_test.sh` | Streams live H.264 fMP4 with the Mac's clock burned in to the TV's :7100 |

## Build

```bash
source ~/vega/env
cd vega/native && npm install && npm run build:debug
cd ../app && npm install && npm run build:debug
```

Fire TV Stick (Vega) is armv7: `app/build/armv7-debug/vegaprobe_armv7.vpkg`.

SDK note: on this Mac `vega sdk install` hangs fetching metadata (the same URL works with curl), so the
SDK was installed from the self-extracting package downloaded with curl, then `vega sdk link`. The
Vega Virtual Device image isn't installed for the same reason; the LAN questions need a real device anyway.

## 周一真机测试清单

准备：电视棒和 Mac、iPhone 在同一个 Wi-Fi；电视和 Mac 都开启网络时间。

1. **开启开发者模式**（需要你在场）
   - 电视：设置 → My Fire TV → About，在设备名上按 7 次确认键 → Developer Mode，记下 6 位码
   - Mac：`vega devmode login`（浏览器里登录你的 Amazon 开发者账号），然后 `vega devmode enable-device --code <6位码>`
   - `vega device list` 能看到设备
2. **安装并启动**
   ```bash
   vega device install-app -p vega/app/build/armv7-debug/vegaprobe_armv7.vpkg
   vega device launch-app -a com.screenatic.probe.main
   vega device start-log-stream
   ```
   屏幕左上显示 Running 和电视的 IP。
3. **端口能否从局域网连进来**（最关键）
   ```bash
   curl http://<电视IP>:7000/
   curl http://<电视IP>:49595/description.xml
   ```
   返回文字、电视上 "Remote peers" 出现 Mac 的 IP = 通过。连接被拒或超时 = Vega 上做不了接收端。
4. **AirPlay 发现**：Mac 上 `dns-sd -B _airplay._tcp` 能看到 "Screenatic Probe"；iPhone 控制中心 → 屏幕镜像列表里出现它（点了会失败，正常——探测版没有实现协议）。
5. **DLNA 发现**：Windows 按 Win+K 或用安卓 DLNA App 搜索，能看到 "Screenatic Probe"。
6. **视频延迟**：`vega/scripts/stream_test.sh <电视IP>`，电视右上应出现彩条和 Mac 时钟。延迟 = 下方 "TV clock" 减去画面里的时钟。镜像可用的目标是 300 ms 以内。
7. **长时间运行**：视频流跑 10 分钟以上，看会不会被系统回收。
8. 拍照或截屏记录各项结果，日志存一份。

还没覆盖：PCM 声音输出（`VAudioStream`，SDK 里有 C 接口，下一步加）；AirPlay 加密握手和 AAC-ELD 解码（等上面通过再移植 UxPlay）。
