# Screenatic on Vega OS (Fire TV)

The Vega app started as a feasibility probe (the test notes below keep that history) and is now the
Screenatic receiver: AirPlay screen mirroring with sound, AirPlay video (URL mode) for Safari
and YouTube, AirPlay music (audio-only: Apple Music etc.), and DLNA (Android apps, Windows "Cast to Device": video, music, photos). Plan and findings: `PORTING.md`.

| Folder | What |
|---|---|
| `native/` | C++ TurboModule (still named `ProbeNative`): mDNS responder, UxPlay (shared with Android, plus `protocol-airplay/patches/`), AAC-ELD → FLAC, native MSSE player for the mirror (`MssePlayer.cpp`), DLNA MediaRenderer on TCP 49595 + SSDP (`DlnaRenderer.cpp`, a port of `protocol-dlna`), user engagement (screensaver) |
| `app/` | React Native for Vega: home screen, full-screen mirror (OK toggles fit / fill), URL video player, update screen, 10 languages (`src/i18n.ts`), debug panel |
| `scripts/stream_test.sh` | Debug: streams live H.264 fMP4 with the Mac's clock burned in to the TV's :7100 |

App id `com.screenatic.receiver` (component `com.screenatic.receiver.main`). The receiver is named
"Screenatic TV" plus the last 4 hex digits of its stored device id (e.g. "Screenatic TV 34DA").

**Debug vs release** (`__DEV__` → `ProbeNative.startReceiver(name, debug)`): release builds start mDNS,
AirPlay, SSDP and DLNA. Debug builds add the :7000 diagnostics server (`/status` with a millisecond
`timeline`, a `dlna` section with the last requests, `/mdns`, `/msse`, `/play`, `/dump.*`), the :7100
ingest, the JS MSE queues, RTSP request tracing and session dumps, and the status panel on the
remote's Menu key.

The update screen appears when `minVegaBuild` in https://screenatic.com/rules.json (`rules-dev.json`
in debug) is above `APP_BUILD` in `app/src/rules.ts` (keep it equal to `build-number` in
`manifest.toml`).

## Build

```bash
source ~/vega/env
cd vega/native && npm install && npm run build:debug
cd ../app && npm install && npm run build:debug
```

Fire TV Stick (Vega) is armv7: `app/build/armv7-debug/screenatic_armv7.vpkg`.

```bash
vega device install-app -p vega/app/build/armv7-debug/screenatic_armv7.vpkg
vega device launch-app -a com.screenatic.receiver.main
```

SDK note: on this Mac `vega sdk install` hangs fetching metadata (the same URL works with curl), so the
SDK was installed from the self-extracting package downloaded with curl, then `vega sdk link`. The
Vega Virtual Device image isn't installed for the same reason; the LAN questions need a real device anyway.

## 周一真机测试清单（探测版，2026-11-23，已完成）

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

### 镜像改用原生 MSSE 播放（2026-11-24，`MssePlayer.cpp`）

MSE 镜像播视频约 1 分钟后定住，原因已查清：系统的 w3cmedia JS 包（系统内置包，App 里改不了）创建 SourceBuffer 时把自动清理阈值写死为 100 MB（`DEFAULT_EVICTION_THRESHOLD`）；清理和 `remove()` 都只能删到关键帧为止，而 iPhone 播视频时几分钟不发关键帧（实测整段只有 1 个 IDR，没有非 IDR 的 I 帧和恢复点 SEI），一删就把整个缓冲清空。所以改成在 C++ 里直接用 W3C Media 的 MSSE（Media Sample Source）模式逐帧注入，播完即释放，没有累积。

| 项目 | 结论 |
|---|---|
| 接口 | `kepler_add_idl_import_library` 从 SDK 的 IDL 生成 C++ 绑定；`getComponent("/com.amazon.kepler.w3cmedia")`，manifest 加 `/com.amazon.kepler.w3cmedia@IW3CMedia` |
| 视频格式 | `video/h264`，**Annex-B 起始码** + avcC 配置。`video/mp4; codecs=avc1…` 报错 4；长度前缀样本不报错但不出画面 |
| 音频格式 | 同一个播放器的第二条轨道，`audio/x-flac` + "fLaC"+STREAMINFO。`audio/flac` 不报错但没声音。iPhone 没声音时不发音频，要补静音帧，否则播放器一直等音频不起播 |
| 顺序 | 先 `addTrack` 再 `setMediaSampleSource`（它会重建管线，之后加的轨道收不到数据）；之后再 `setVideoSurface`；`initializeAsync` 回调前不能调其他接口 |
| 画面 | 用 JS `KeplerVideoSurfaceView` 的 view tag（`findNodeHandle`）→ `IUIManager::tryGetViewFromTag`，在它下面用 `IViewModule::makeMediaSurfaceView` 建一个媒体画面视图，取其 `IMediaSurface`。自己 `createMediaSurface`/建窗口都会被界面盖住；调用系统内部的 `notifySurfaceBound` 会崩溃 |
| 时间戳 | 用帧到达电视的时间；播放器时钟一直按真实时间走，跳过静止空档的时间轴会落后，帧被当成迟到全部丢弃。起播前攒 100 ms，卡顿后每帧回收 2 ms 延迟 |
| 链接模式 | Safari/YouTube 全屏时 MSSE 让出解码器（释放后再重建）；iPhone 交出地址后立刻断开镜像连接，这个断开要忽略 |
| 线程 | 不要在持锁时调用播放器接口：播放器一卡，状态接口、HTTP、mDNS 全被拖住，iPhone 找不到电视 |
| 结果 | 相册视频连续 7 分钟以上不卡、音画同步；Safari 全屏正常 |

调试接口：`GET :7000/msse?mime=&annexb=&config=&amime=&aconfig=&audio=&cushion=&suspend=` 运行时切换格式并重建播放器；`:7100` 收到以 `SCRV` 开头的数据时按"长度+时间戳+H.264 访问单元"走镜像同一路径（`scripts/replay_annexb.py` 用）。日志只保留约 100 行，系统崩溃报告用 `vega device copy-logs -a SYSTEM_TOMBSTONE/acr`。屏保会以 `ReclaimMediaResource` 杀掉后台 App。

调试中踩到的坑（正式移植时要注意）：
- 设备默认是 USB 调试：Developer Options → Connection Mode 改 TCP/IP 后用 `vega exec vda connect <ip>:5555`。vda 和 adb 都用 5037 端口，用前 `adb kill-server`。
- `file:../native` 链接的 TurboModule 要在 `metro.config.js` 里加 `watchFolders`，否则运行时报 "Cannot find module"。
- w3cmedia 需要先设置全局 `navigator`/`window`/`self`，否则 `VideoPlayer.initialize()` 永远不返回。
- 不能用 `src = URL.createObjectURL(ms)`（播放器只认 `blob:///<id>` 格式），改用 `srcObject = ms`；而且 `sourceopen` 在 `srcObject` 赋值时同步触发，必须先挂监听。
- 播放器没就绪前收到的数据要限量缓存，否则内存涨到约 27 MB 时 App 会被系统杀掉。
- Vega 的布局是 960×540 dp，写死宽度会挤坏界面。
- 每个 App 的日志只保留约 100 行，w3cmedia 每次查询 `buffered` 都会打一行；调试时少轮询，或者把状态显示在屏幕上。设备自带的 `screenshooter` 截图会卡住。

### 镜像连接从 5 秒到 1 秒（2026-11-24，`ProbeNative.cpp` mDNS）

现象：在 iPhone 上点 Screenatic 后要等约 5 秒才出画面，FireMirror 不到 1 秒。用 `GET :7000/status` 里的 `timeline`（毫秒时间线，记录 mDNS 查询、RTSP 请求、第一帧和播放器事件）排查后发现：

- 电视端很快：从 TCP 连接到开始播放约 0.7 秒。慢的 5 秒发生在 iPhone 连接之前，期间 iPhone 没有发出任何请求。
- **原因**：`features` 里的 bit 27（SupportsLegacyPairing，UxPlay 默认带）。FireMirror 不广播这一位。去掉之后 iPhone 打开面板后约 1 秒就连上，配对流程（pair-setup / pair-verify）、画面和声音都不受影响。代码见 `withoutLegacyPairing()`，`_airplay` 的 `features` 和 `_raop` 的 `ft` 都要改。
- 顺带修了几处：设备 ID 和 AirPlay 密钥改存到 `/data`，因为 `/tmp` 每次启动都会清空，导致电视每次都像一台新设备；补上 `_raop._tcp` 广播；主机名加了 NSEC 记录，表示只有 IPv4，iPhone 不用再等 AAAA 查询。
- 不要广播 IPv6 地址：iPhone 会改走 IPv6 连接，结果镜像没有声音，而且连接也没变快。
- 调试：`GET :7000/mdns?key=value&...` 可以实时改广播的 TXT（`features` / `flags` / `srcvers` 会同步到 RAOP 记录），不带参数则恢复默认。

### Safari 全屏进出与切换 App 时的声音（2026-11-24）

- **退出全屏后画面定格、声音断续**：iPhone 退出全屏时有时只发 `rate=0`（暂停），不发 `/stop`，网页视频播放器因此一直占着解码器。现在镜像一恢复（收到 mirror 的 `connected`），`UrlPlayer` 就停掉网页播放器。
- **声音一顿一顿**：镜像音频的时间戳以前最多可以比到达时间晚 200 ms，可画面只比到达时间晚约 150 ms，声音于是落到播放进度后面。现在声音只要落后超过 30 ms，就跳到到达时间 +30 ms，中间用静音补上（`MssePlayer::pushAudio`）。
- **切换 App 后声音晚出来约 1 秒**：iPhone 不出声时会发带序号的"无数据"占位包，UxPlay 把它们当成丢包，先请求重传（2 秒内回来上万个包），再每收到一个包才跳过一个序号。补丁 `0005-audio-skip-silence-gaps.patch` 把占位包的序号直接标记为空，遇到丢包时一次跳过整段空缺。修复后切换体验与 FireMirror 相同。
- 调试：`timeline` 里每 2 秒记录一次 `audio udp:`（补丁 `0004`：收包数、进入缓冲区的包数、丢弃数、播放数）、`audio rtp:`（RTP 序号缺口）、`audio level:`（音量调节前的电平和音量系数），另外还有 `TEARDOWN request, 96=…, 110=…`（96 是音频流，110 是镜像画面）。这些都只在 `traceRequests` 打开时输出。

### 正式版：全屏、1080p、声音（2026-11-25）

探测版改成正式 App 后，在电视上又测出这几处，都已修好：

| 现象 | 原因 | 处理 |
|---|---|---|
| 镜像有声音没画面 | 竖屏时 JS 把画面视图缩到约 250 宽，原生的媒体画面视图还是 960×540，画面画在了被裁掉的区域 | 每次布局变化后重新 attach，原生只把自己的画面视图调成同样大小 |
| 文字发虚 | 请求的是 720p，竖屏只有 334×720 | 改请求 1080p（竖屏约 500×1080）；MSSE 没有缓冲上限 |
| Safari 全屏约 1 分钟后定住 | 中途的尺寸变化触发 attach，把镜像播放器重建出来，抢走了网页播放器的解码器 | 解码器交出去期间，attach 只调尺寸 |
| Safari 全屏播几分钟后偶尔定住（readyState 2） | Vega 播放器卡在 HLS 缓冲里出不来 | 8 秒不动就重新加载，并跳回原位置（实测 5 分钟左右卡过两次，几秒后自动恢复） |
| 从 Safari 回来、重连、手机静止后，镜像有画面没声音 | 音轨空了约 70–200 ms（补静音要等 300 ms 才开始），Vega 的音频输出碰到断档就再也不出声 | 独立线程一发现音频跟不上就立刻补静音；每次新的镜像连接都重建播放器 |

调试方法：`timeline` 里会记 `msse audio ran dry for N ms`（音轨断档）和 `audio level`（声音电平和音量）；`GET :7000/msse?audio=1` 可以当场重建播放器，用来判断是不是播放器本身的问题。安卓版的同一现象（Safari 回来后相册无声）原因不同：iPhone 第二次发音频 SETUP 时，电视回的端口是 0。补丁 `0006-audio-setup-running-ports.patch` 修复了这一问题，Vega 也打上了这个补丁（Vega 用固定端口，原本就不受影响）。

### DLNA（2026-11-25）

`DlnaRenderer.cpp` 是安卓版 `protocol-dlna` 的 C++ 移植（SSDP、设备描述、AVTransport / RenderingControl / ConnectionManager、GENA 事件），发布版也启动。播放走 JS 的 `UrlPlayer`：视频和音乐用 Vega 播放器（音乐显示封面和歌名），照片用 `Image`。事件和 AirPlay 共用 `pollEvents` 队列（`"source":"dlna"`），JS 用 `setPlayback("dur,pos,rate,ready,buffering,ended,failed,source,closed")` 回报状态。

- Windows "投射到设备"：视频能播。关掉 Windows 播放器时它只取消订阅、不发 Stop，所以控制端 10 秒没有请求时电视回首页（其他控制端只在暂停/停止时，20 秒）。
- 未解决：播放中关掉 Windows 播放器后，Windows 一直显示"正在断开连接"，直到注销；它之后不再给电视发任何请求，SSDP byebye/alive 也没用。安卓版同样如此。
- 安卓手机投视频、音乐、照片还没实测。

### AirPlay 纯音频（2026-11-25）

音乐 App（Apple Music 等）走 AirPlay 纯音频：ct=2 ALAC（Apple 的解码器，和安卓版同一份）或 ct=4 AAC-LC（FFmpeg），解码后和镜像声音一样转成 FLAC（统一按 480 采样一帧，和播放器的 STREAMINFO 一致）。`MssePlayer` 的"只有音轨"模式：音乐会话连上后用第一个音频帧建一个不带视频的播放器，攒 100 ms 后起播；和镜像会话互相切换时重建播放器。补静音的线程照常工作，暂停、切歌不会断档。

歌曲信息、封面（data: URI）、进度和播放/暂停通过事件交给 JS（`Music.tsx`，界面和 DLNA 音乐共用 `NowPlaying.tsx`）。实测：Apple Music 播放、拖进度、上一首/下一首、音量正常；ALAC 0 解码错误，CPU 约 4%；停止后回首页，之后镜像正常。

### 后台（2026-11-25）

按 Home 或切到别的 App 时，Vega 保留进程，mDNS、AirPlay、DLNA 都还在运行，但只要一开始播放媒体，系统就会立刻杀掉后台 App。App 也无法自己回到前台：后台的 JS 被冻结，从 App 进程里发出的 `pkg://` 启动请求会被拒绝（`forbidden by launch policy`，电视上显示 "Unable to load app"）。所以 App 进入后台时会交出镜像播放器（镜像只保留最近一个关键帧之后的画面，回到前台时补放），停止 SSDP 和 mDNS 广播（分别发送 byebye 和 TTL 为 0 的 goodbye），手机列表里不再显示这台电视。回到前台后恢复播放器和广播。正在播放的 Safari 或 DLNA 视频，进入后台时会停止。

调试时可以用 `vlcm trigger-app background|foreground --inst <id>` 切换前后台，`id` 从 `vlcm list` 里查。注意 `vega exec vda` 同时连着多台设备时，要用 `-s <ip>:5555` 指定设备。
