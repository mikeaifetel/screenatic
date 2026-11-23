# Screenatic for Vega OS：移植方案

适用设备：Fire TV Stick 4K Select 等运行 Vega OS 的 Fire TV（armv7 / aarch64，Linux + glibc）。
依据：`vega/README.md` 里的真机测试结果（2026-11-23）。

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
| 1. 镜像视频 | mDNS + UxPlay + H.264 fMP4 + MSE | iPhone 能连上并显示画面，延迟 < 0.5 秒，横竖屏切换正常 |
| 2. 镜像声音 | AAC-ELD 解码 + FLAC + 第二个 track | 有声音、口型对得上，10 分钟不断音 |
| 3. UI 和规则 | 首页、连接中、正在播放、10 种语言、时长规则（从 screenatic.com 拉配置） | 和安卓版界面一致 |
| 4. 一期发布 | 打包 vpkg、商店素材、隐私政策（复用 screenatic.com）、提交 Amazon Appstore | 审核通过 |
| 5. DLNA | 二期 | Windows Win+K、安卓 BubbleUPnP 能投视频、音乐、照片 |
| 6. URL 和纯音频 | 三期 | YouTube / Safari 投视频、Apple Music 投音乐 |

**阶段 0 的新发现**：Vega 挡住局域网到系统临时端口（32768 以上）的入站连接。UxPlay 默认让系统选端口，所以 Vega 版改用固定端口 7200–7204（`AirPlayOptions::fixedPortBase`，安卓版不受影响）。正式版的 DLNA 等所有监听端口都要用固定的低端口。

## 6. 风险和待确认的问题

| 问题 | 影响 | 怎么确认 |
|---|---|---|
| App 离开前台就被关掉 | 用户在投屏中按 Home 键，连接会断；不能像安卓版那样后台接收 | 查 Vega 是否允许 interactive 组件常驻；做不到就在 UI 上说明 |
| ~~FFmpeg AAC-ELD 解码在 armv7 上的 CPU 占用~~ | 已排除：实测解码 + FLAC 编码只占一个核的 2.7% | — |
| JS 数据通道在高码率下的吞吐 | 镜像卡顿 | 阶段 1 用 15 Mbps 测试流压测 |
| MSE 在分辨率变化时的表现 | 横竖屏切换花屏或卡住 | 阶段 1 实测 |
| Vega 的 HEVC 支持 | 只影响画质上限，不影响可用性 | 先只用 H.264 |
| Appstore 对"接收其他设备投屏"类 App 的审核 | 能否上架 | 一期前读 Amazon 的 Vega 应用政策 |

## 7. 工作量估计

按"做着玩"的节奏，以每期能发布为单位：
- 一期（阶段 0–4）：最大的一块，主要难点是构建链和音频转码。
- 二期 DLNA：中等，主要是 Kotlin → C++ 的翻译和播放兼容性测试。
- 三期：取决于 HLS 播放器在 Vega 上的适配难度。

建议先做阶段 0：它决定了 UxPlay、FFmpeg、libFLAC 能不能在 Vega 工具链下顺利编译。这一步通过后，剩下的都是已经验证过可行的工作。
