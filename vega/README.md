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

## 测试结果（2026-11-23，Fire TV Stick 4K Select，Vega OS 2.1 TV Ship/534，armv7）

| 项目 | 结果 |
|---|---|
| 局域网连入 App 监听的 TCP 端口（7000 / 49595 / 7100） | ✅ 可以。Mac、手机、电脑都连进来了，App 不需要额外权限 |
| mDNS 广播 `_airplay._tcp`（5353 与系统响应器共用） | ✅ Mac `dns-sd` 能解析出完整 TXT；iPhone 屏幕镜像列表出现 "Screenatic Probe" |
| SSDP MediaRenderer（1900 与系统共用） | ✅ 应答 M-SEARCH，局域网设备会来读 description.xml |
| MSE 视频（H.264 fMP4 → SourceBuffer） | ✅ 能播放。延迟在 0.1–0.5 秒量级；测试方法本身有几百毫秒误差（ffmpeg 起始偏移、两台设备时钟差），精确值要改进测法 |
| 3 分钟连续推流 | ✅ 不断流、App 不被回收 |
| 12 分钟连续推流（App 在前台） | ✅ 不断流、App 不被回收。离开 App（比如进设置）时 App 会被关掉 |
| 原生 PCM 输出（`VAudioStream` C 接口） | ❌ 用不了：SDK 2.0.15159 的头文件依赖 `Vntk/Vntk.h`，SDK 里没有，CMake 也缺 `vntk_api_declare` |
| MSE 音频 `isTypeSupported` | AAC-LC、HE-AAC、FLAC、Opus、LPCM、ipcm 返回 yes；**AAC-ELD、ALAC、AC-3 返回 no** |
| FLAC 音频（与 H.264 同一个 SourceBuffer） | ✅ 实测出声，缓冲领先 0.2 秒 |
| LPCM（`ipcm`） | ❌ `isTypeSupported` 说支持，实测没声音（视频照常） |

**音频方案结论**：AirPlay 的 AAC-ELD（镜像）和 ALAC（纯音频）都不能直接交给 MSE。Vega 版需要在原生代码里解码成 PCM，再编码成 FLAC（libFLAC，BSD 许可，与 GPL 兼容），和 H.264 一起封装成 fMP4 交给 MSE。`stream_test.sh` 默认就推这种流（视频 + 带整秒提示音的 FLAC），`AUDIO=pcm` 可以复现 LPCM 无声的情况。

阶段 0（`scripts/build-deps-vega.sh` + UxPlay 链接进 ProbeNative，App 启动时调用 `airplayStart()`）：

| 项目 | 结果 |
|---|---|
| 依赖库（OpenSSL、libplist、FFmpeg aac、libFLAC）× armv7 / aarch64 / x86_64 | ✅ 用 Vega SDK 的 clang + glibc sysroot 编译通过 |
| UxPlay 在电视棒上启动、mDNS 广播真实 TXT | ✅ |
| iPhone 屏幕镜像连接（配对、加密、视频流） | ✅ 1611 帧、37 MB、500×1080（竖屏） |
| 镜像音频 AAC-ELD → PCM → FLAC | ✅ 11 秒 0 错误，CPU 2.7%（解码 205 ms + FLAC 93 ms） |
| 系统分配的端口（≥ 32768）从局域网连入 | ❌ 被拒绝；改用固定端口 7200–7204 后正常 |

阶段 1、2（镜像画面和声音，详见 PORTING.md）：画面流畅、有声音、静止界面会更新；全屏播放视频约 1.5–2 分钟后画面定住（Vega 播放器约 100 MB 的缓冲上限 + iPhone 播视频时不发关键帧）。远程诊断：`curl http://<电视IP>:7000/status`。

AirPlay 视频链接模式（2026-11-23，`UrlPlayer.tsx`，独立的 VideoPlayer + 画面）：

| 来源 | 发来的内容 | 结果 |
|---|---|---|
| YouTube | UxPlay 在本机提供的 HLS 播放列表（`http://<电视IP>:7200/master.m3u8`） | ✅ 能长时间播放（补丁 0003 把地址从 localhost 改成局域网 IP） |
| Safari 网页视频 | 网站的 HLS 地址 | ✅ 能播，音量跟随 iPhone |
| 系统相册（本地视频） | 不发地址，走镜像 | ❌ 不出画面或很快定住，原因未查清（见下） |
| 百度网盘 | 找不到电视 | 多半用 DLNA 或 `_raop._tcp`，探测版没有 |

- Vega（RN 0.83）默认**拦截所有明文 http**，播放器不报错也不发请求，一直停在加载中。manifest 里加 `[network-traffic-policy.cleartext] allowed-domains = ["*"]` 后，局域网 http 的 MP4 和 HLS 都能播。DLNA 也需要它。
- 文档说 URL 模式不支持 HLS，实测支持（底层是 GStreamer）。
- 屏保会把 App 关掉（等待投屏时没有播放），设置里找不到关闭屏保的选项，正式版要解决。开发时可用 `vega exec vda shell "inputd-cli button_press KEY_LEFT"` 远程按键。
- 相册视频镜像：同一段流在 Mac 上重放给电视棒能播（但丢帧约 75%），实时镜像时却只解码 1–86 帧就停住；色彩标记（全范围 + sRGB）不是原因。调试接口：`GET :7000/dump.h264|mp4|pts`（会话开头 20 MB）、`GET :7000/play?url=...`。
- 镜像缓冲在没有关键帧时也会"直接删 10 秒前的数据"（实验中，效果未验证）。

调试中踩到的坑（正式移植时要注意）：
- 设备默认是 USB 调试：Developer Options → Connection Mode 改 TCP/IP 后用 `vega exec vda connect <ip>:5555`。vda 和 adb 都用 5037 端口，用前 `adb kill-server`。
- `file:../native` 链接的 TurboModule 要在 `metro.config.js` 里加 `watchFolders`，否则运行时报 "Cannot find module"。
- w3cmedia 需要先设置全局 `navigator`/`window`/`self`，否则 `VideoPlayer.initialize()` 永远不返回。
- 不能用 `src = URL.createObjectURL(ms)`（播放器只认 `blob:///<id>` 格式），改用 `srcObject = ms`；而且 `sourceopen` 在 `srcObject` 赋值时同步触发，必须先挂监听。
- 播放器没就绪前收到的数据要限量缓存，否则内存涨到约 27 MB 时 App 会被系统杀掉。
- Vega 的布局是 960×540 dp，写死宽度会挤坏界面。
- 每个 App 的日志只保留约 100 行，w3cmedia 每次查询 `buffered` 都会打一行；调试时少轮询，或者把状态显示在屏幕上。设备自带的 `screenshooter` 截图会卡住。
