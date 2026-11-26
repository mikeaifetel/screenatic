# Screenatic for Vega OS：移植方案

适用设备：Fire TV Stick 4K Select 等运行 Vega OS 的 Fire TV（armv7 / aarch64，Linux + glibc）。
依据：`vega/README.md` 里的真机测试结果（2026-11-23 起）。最近更新：2026-11-25，正式版 App 跑通。

## 1. 目标和范围

和安卓版功能对齐，按价值分三期，每期都能单独发布：

| 期 | 功能 | 说明 |
|---|---|---|
| 一期（MVP） | **AirPlay 屏幕镜像**（iPhone / iPad / Mac），带声音 | 用户最常用、竞品最少的场景 |
| 二期 | **DLNA**（安卓、Windows 投视频 / 音乐 / 照片） | 播放走系统播放器的 URL 模式，不经 MSE |
| 三期 | **AirPlay 视频 URL**（App 里点 AirPlay 投视频）、**AirPlay 纯音频**（音乐） | 需要 HLS 播放器和 ALAC → FLAC |

不在范围内：后台继续接收（Vega 在 App 离开前台时会关掉 App，见第 6 节）、广告、Miracast。

## 2. 整体架构

```
iPhone ──AirPlay──▶ C++ 原生模块（TurboModule "ScreenaticNative"）
                      ├─ mDNS 广播 _airplay._tcp / _raop._tcp（沿用探测版的自写响应器）
                      ├─ UxPlay 核心（配对、加密、RTSP、RTP）←── 和安卓版同一份 airplay_server.cpp
                      ├─ 视频：H.264 NAL + 时间戳 ──┐
                      ├─ 音频：AAC-ELD ─▶ 解码 PCM ─▶ FLAC 编码 ─┤
                      └─ fMP4 封装（moov + moof/mdat，视频 + 音频两个 track）◀┘
                                     │  ArrayBuffer（readMedia，和探测版的 readVideo 一样）
                                     ▼
                     React Native（JS）── W3C VideoPlayer + MediaSource（MSE）── 屏幕 / 扬声器
                                     └─ UI：首页、正在播放、照片、更新提示、规则（时长额度）
```

为什么这样分：
- Vega 的 JS 不能开 socket，所有网络协议都必须在 C++ 里。
- Vega 没有可用的原生音视频输出（`VAudioStream` 编不了，见 README），唯一的出口是 W3C 媒体播放器。
- 播放器只认 H.264 + FLAC（实测），所以音频必须先在 C++ 里转成 FLAC。

## 3. 代码复用

| 安卓版模块 | Vega 版 | 工作量 |
|---|---|---|
| `protocol-airplay/src/main/cpp/airplay_server.{h,cpp}`（UxPlay 封装，回调接口） | **原样复用**，提到 `shared/` 两边共用 | 小 |
| UxPlay 子模块 + `patches/`（URL 播放、音频重发等待） | 原样复用，用 Vega 工具链编译 | 小 |
| `scripts/build-deps.sh`（OpenSSL、libplist） | 加 Vega 目标：Vega 用的是 Linux/glibc 工具链（`VodkaClangArmv7`），比 NDK 更接近 UxPlay 的原生环境 | 中 |
| ALAC 解码（Apple 开源，Apache-2.0） | 原样复用 | 小 |
| `audio_player.cpp`（MediaCodec + Oboe） | **重写**：PCM → FLAC → fMP4，见第 4 节 | 中 |
| `video_decoder.cpp`（MediaCodec + Surface） | **重写**：H.264 → fMP4 → MSE | 中 |
| `jni_bridge.cpp` | 换成 TurboModule（探测版已经跑通这套写法） | 小 |
| `AirPlayAdvertiser.kt`（安卓 NsdManager） | 用探测版的 C++ mDNS 响应器（已验证 iPhone 能看到） | 小 |
| `protocol-dlna`（Kotlin，约 1200 行：SSDP、HTTP、SOAP、GENA） | 翻译成 C++（探测版已有 SSDP 和 HTTP 骨架） | 中 |
| `core`（会话、时长规则 CastTimeRules，约 600 行 Kotlin） | 翻译成 TypeScript | 小 |
| `app/ui`（Compose，约 600 行） | 用 React Native 重写；10 种语言字符串从 `strings.xml` 转成 JSON | 中 |

## 4. 关键技术点

### 4.1 视频：H.264 → fMP4 → MSE
- UxPlay 的 `onVideo` 回调给出 Annex-B 格式的 NAL 单元和时间戳。C++ 里做一个最小的 fMP4 封装器：
  - 收到 SPS/PPS 时生成 `moov`（`avcC`）；
  - 每帧生成一个 `moof` + `mdat`（AVCC 长度前缀）。
- 只广播 H.264，不广播 H.265。Vega 的 MSE 是否支持 HEVC 还没测；UxPlay 已有开关。
- 低延迟：沿用探测版的做法，`AppendMode.sequence`，播放落后超过 0.5 秒就跳到最新位置。实测延迟零点几秒。
- 分辨率变化（手机横竖屏切换）时 SPS 会变：要重新发 `moov`，或者重建 SourceBuffer。

### 4.2 音频：AAC-ELD → PCM → FLAC → fMP4
- **AAC-ELD 解码器**：Vega 上没有系统解码器，要自带一个，而且许可证要和 GPL 兼容。
  - 首选 **FFmpeg 的 `aac` 解码器**（LGPL-2.1+，支持 ER AAC ELD）。只编译 libavcodec 的这一个解码器，体积约几百 KB。
  - 不能用 fdk-aac：它的许可证和 GPL 不兼容。
- **FLAC 编码**：libFLAC（BSD）。用最低压缩级别、每帧 480 采样（和 AAC-ELD 每包的采样数一致），CPU 占用很低。
- **封装**：音频作为第二个 track 和视频放进同一个 fMP4（实测这种方式能出声）。音视频的时间戳都来自 UxPlay 的 NTP 时间，用来对齐口型。
- 音量：AirPlay 的音量命令在 PCM 阶段乘增益（沿用安卓版 `setVolumeDb` 的曲线）。
- 断流处理：沿用安卓版的修复（UxPlay 补丁 0002：最多等 24 个包）。

### 4.3 C++ → JS 的数据通道
- 探测版用 30 ms 轮询 `readVideo()` 取 ArrayBuffer，3 分钟传了 90 MB，没问题。
- 镜像码率一般 2–8 Mbps，峰值可能到 15 Mbps。正式版要测高码率下 JS 线程的占用。
  如果不够，改成由原生模块主动推事件，或者把轮询间隔降到 16 ms。

### 4.4 DLNA（二期）
- SSDP、HTTP、SOAP（AVTransport / RenderingControl / ConnectionManager）、GENA 全部在 C++ 里，逻辑照搬 Kotlin 版。
- 播放：把媒体 URL 交给 W3C `VideoPlayer`（`src = url`），不经 MSE。照片用 React Native 的 `Image`。
- 要实测：Vega 播放器直接播 HTTP 上的 MP4、MP3、FLAC 的兼容性，以及跳转（seek）是否正常。

### 4.5 AirPlay 视频 URL 和纯音频（三期）
- URL 模式（YouTube、Safari 等）：iPhone 给的是 HLS 地址。Vega 官方文档推荐用 Shaka 或 hls.js 跑在 MSE 上，需要按文档做几处兼容改动。
- 纯音频（音乐 App）：ALAC → PCM → FLAC → fMP4，走和镜像音频同一条路，只是没有视频 track。

## 5. 分期计划和验收标准

| 阶段 | 内容 | 验收 |
|---|---|---|
| 0. 构建打通 ✅ | Vega 工具链编译 OpenSSL、libplist、UxPlay、FFmpeg（仅 aac 解码）、libFLAC；链接进 TurboModule | ✅ 2026-11-23：UxPlay 在电视棒上运行，iPhone 镜像配对成功并收到视频（1611 帧、37 MB）；AAC-ELD 解码 + FLAC 编码 11 秒音频 0 错误，CPU 占用 2.7% |
| 1. 镜像视频 ✅ | mDNS + UxPlay + H.264，改走原生 MSSE（`MssePlayer.cpp`） | 2026-11-23 的 MSE 版本看视频 1.5–2 分钟后卡死（见下文）；2026-11-24 改成原生 MSSE 逐帧注入后解决。2026-11-25：请求 1080p，相册视频连续 5 分钟以上正常 |
| 2. 镜像声音 ✅ | AAC-ELD 解码 + FLAC，作为 MSSE 播放器的第二条音轨 | 2026-11-23：有声音，音量记忆（首次 -15 dB）。2026-11-25：修好 Safari、重连、手机静止之后"再也不出声"的问题（见下文） |
| 3. UI 和规则 ✅ | 首页、全屏镜像（OK 键切换适应/填充）、10 种语言、更新提示（`rules.json` 的 `minVegaBuild`） | 2026-11-25 在电视上跑通。Vega 版没有广告，时长规则（metering）不生效 |
| 4. 一期发布 | 打包 vpkg、商店素材、隐私政策（复用 screenatic.com）、提交 Amazon Appstore | 审核通过 |
| 5. DLNA | 二期 | Windows Win+K、安卓 BubbleUPnP 能投视频、音乐、照片 |
| 6. URL 和纯音频 | 三期 | YouTube / Safari 投视频、Apple Music 投音乐 |

**阶段 0 的新发现**：Vega 挡住局域网到系统临时端口（32768 以上）的入站连接。UxPlay 默认让系统选端口，所以 Vega 版改用固定端口 7200–7204（`AirPlayOptions::fixedPortBase`，安卓版不受影响）。正式版的 DLNA 等所有监听端口都要用固定的低端口。

**阶段 1、2 的发现（决定了 Vega 版的定位）**

- Vega 播放器停下后要攒到约 0.5 秒缓冲才恢复，所以缓冲余量至少留 0.6 秒；落后时用 1.2 倍速追（1.5 倍时 1080p 解码跟不上）。不能用跳转追帧：跳转后要等下一个关键帧。
- 静止界面几乎不发帧：静止后的第一帧时长最多记 0.6 秒，否则攒不够 0.5 秒，画面不刷新。
- 播放器缓冲上限约 100 MB，满了以后 `appendBuffer` 一直处于 updating，画面定住。只能在关键帧处删除旧数据（删到别处会连带删掉依赖它的帧，画面也没了）。
- iPhone 普通界面约每 10 秒一个关键帧，可以按关键帧清理；**全屏播放视频时几乎不发关键帧**。1080p 约 14 Mbps，约 1 分钟撑满；改请求 720p 后约 8 Mbps，约 1.5–2 分钟撑满。
- AirPlay 协议里接收端无法请求关键帧；断开视频数据连接后 iPhone 也不会重连（实测，iPhone 继续"镜像"，电视画面停住）。
- 同一个播放器换新的 MediaSource 在 Vega 上不可靠（新缓冲始终为 0），所以整个会话共用一个 SourceBuffer。
- 音频单独走 AudioPlayer：每个 FLAC 帧都能独立解码，可以随时删除旧数据、落后时直接跳转。

**结论（2026-11-23，已过时）**：Vega 版的镜像用于手机界面、照片、演示；看视频引导用户用 App 里的 AirPlay 按钮（视频链接模式，三期），由 Vega 播放器直接播网络视频，没有这个限制。三期的视频链接模式提前到一期做（2026-11-23 已在探测版跑通 YouTube 和 Safari，见 README）。彻底解决全屏视频镜像需要原生解码接口（SDK 里的 `IMediaTransform` 没有文档，第三方能否使用未知）。

**后来的结果**：2026-11-24 改用 W3C Media 的原生 MSSE 模式（C++ 里逐帧送进播放器，播完即释放），没有 100 MB 缓冲的问题，全屏视频镜像可以长时间播放。上面这些 MSE 限制只在调试用的 JS MSE 路径上还存在。详见 README。

**正式版的发现（2026-11-25）**

- **镜像音轨不能断档**：iPhone 的声音一暂停（静止界面、切换 App、Safari 交接、断开重连），音轨就可能空出几十到两百毫秒。Vega 的音频输出碰到一次断档就再也不出声，画面却照常，只有重建播放器才能恢复。现在用一个独立线程每 50 ms 检查一次：排队的音频一跟不上到达时间，就立刻补静音（只补到当前时间 +40 ms，保持口型同步），不再等 300 ms；另外每次新的镜像连接都重建播放器。
- **Safari 全屏期间不能重建镜像播放器**：这时解码器交给了网页播放器，一旦重建，镜像播放器就会把解码器抢回来，网页视频定住。画面尺寸变化后的重新 attach 只调整画面视图的大小。
- **网页播放器先释放完，再交回解码器**：等 `deinitialize()` 结束（最多等 1.5 秒）再恢复镜像。
- **网页视频偶尔卡在缓冲里**（readyState 2，位置不动）：8 秒没动就重新加载，并跳回原来的位置。
- **1080p**：720p 是当初为了减轻 MSE 缓冲问题才定的，竖屏画面（334×720）文字发虚。MSSE 没有缓冲上限，所以改成 1080p。
- **屏保**：镜像期间用 User Engagement 接口（`makeVideoPlaybackUserEngagement`）告诉系统"正在观看"。
- **接收端名字**：在名字后面加上设备 ID 的后 4 位（如 "Screenatic TV 34DA"），这样同一网络里的多台电视能区分开。安卓版用的是同样的规则。

## 6. 风险和待确认的问题

| 问题 | 影响 | 怎么确认 |
|---|---|---|
| 后台接收 | 已查清（2026-11-25）：按 Home 后进程留在后台，网络服务照常运行；但后台一开始播放媒体就被系统杀掉（`onBackground Kill … for using session media-playback`），而 App 不能把自己调回前台（后台 JS 冻结；从 App 进程发 `pkg://` 启动请求被拒：`forbidden by launch policy`）。所以后台时交出播放器、停止 mDNS/SSDP 广播（发 goodbye/byebye），回到前台再恢复；用户投屏前要先打开 App | 商店说明里写"投屏前先打开 Screenatic" |
| ~~FFmpeg AAC-ELD 解码在 armv7 上的 CPU 占用~~ | 已排除：实测解码 + FLAC 编码只占一个核的 2.7% | — |
| App 等待投屏时被屏保关掉 | 投屏前用户得重新打开 App | 镜像期间已用 User Engagement 挡住屏保；空闲等待时会不会被屏保关掉还要观察 |
| ~~相册本地视频只能走镜像，Vega 上不出画面~~ | 已解决：MSSE 后相册视频正常 | — |
| ~~JS 数据通道在高码率下的吞吐~~ | 已排除：1080p 14 Mbps 每秒 30 帧，1000 多帧只丢 3 帧 | — |
| MSE 在分辨率变化时的表现 | 已验证：同一个 SourceBuffer 接受新的初始化段，竖屏 500×1080 切到横屏 1280×720 正常 | — |
| ~~全屏视频镜像的缓冲上限~~ | 已解决：改用原生 MSSE | — |
| Vega 的 HEVC 支持 | 只影响画质上限，不影响可用性 | 先只用 H.264 |
| Appstore 对"接收其他设备投屏"类 App 的审核 | 能否上架 | 一期前读 Amazon 的 Vega 应用政策 |

## 7. 工作量估计

按"做着玩"的节奏，以每期能发布为单位：
- 一期（阶段 0–4）：最大的一块，主要难点是构建链和音频转码。
- 二期 DLNA：中等，主要是 Kotlin → C++ 的翻译和播放兼容性测试。
- 三期：取决于 HLS 播放器在 Vega 上的适配难度。

建议先做阶段 0：它决定了 UxPlay、FFmpeg、libFLAC 能不能在 Vega 工具链下顺利编译。这一步通过后，剩下的都是已经验证过可行的工作。
