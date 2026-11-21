# 上架 Google Play 指南

## 文件一览

| 用途 | 文件 |
|---|---|
| 上传包（AAB） | `app/build/outputs/bundle/googleRelease/app-google-release.aab`（构建：`./gradlew :app:bundleGoogleRelease`） |
| 应用图标 512×512 | `store/graphics/icon-512.png` |
| 置顶大图 1024×500 | `store/graphics/feature-1024x500.png` |
| 电视横幅 1280×720 | `store/graphics/tv-banner-1280x720.png` |
| 电视截图 1920×1080 | `store/screenshots/en-*.png`（英文及其他语言），`store/screenshots/zh-*.png`（简体中文） |
| 商店文案（10 种语言） | `store/listing/<语言>.md`：标题、简短说明、完整说明 |
| 隐私政策 | `https://screenhop.app/privacy`（中文：`https://screenhop.app/zh/privacy`） |

图形素材的源文件是同目录下的 `.svg`，用 Chrome 无头模式按精确尺寸渲染。

## 上传密钥（务必备份）

- 位置：`~/.screenhop/upload-keystore.jks`，密码在 `~/.screenhop/keystore.properties`（只在这台电脑上，不在仓库里）。
- 请把这两个文件存进密码管理器或其他安全的备份位置。
- 使用 Play 应用签名：Google 保管真正的应用签名密钥，这把只是"上传密钥"。万一丢失，可以在 Play Console 申请重置，但需要几天时间。
- 构建时自动读取；换电脑时把这两个文件放到同样位置即可（或设置环境变量 `SCREENHOP_KEYSTORE_PROPERTIES`）。

## Play Console 操作步骤

1. **创建应用**：名称 `ScreenHop: Phone & PC Mirror`，默认语言 English (United States)，类型"应用"，免费。
   包名是 `app.screenhop.receiver`，第一次上传后就**不能再改**。
2. **设置 → 高级设置 → 设备类型**：添加 **Android TV**（按提示同意电视应用的质量要求）。电视横幅和电视截图在这一步之后才会出现在商店素材里。
3. **主要商品详情**：按语言逐个添加（en-US、zh-CN、zh-TW、ja-JP、ko-KR、es-ES、pt-BR、fr-FR、de-DE、it-IT），从 `store/listing/` 复制文案。
   - 图标、置顶大图、电视横幅：所有语言共用。
   - 电视截图：简体中文用 `zh-*.png`，其余语言用 `en-*.png`。
   - 手机截图：不需要（应用声明了只支持电视）。
4. **应用类别**：工具（Tools）。联系邮箱：填一个你愿意公开的邮箱（会显示在商店页面）。网站：`https://screenhop.app`。

## 应用内容（政策问卷）参考答案

| 问卷 | 答案 |
|---|---|
| 隐私政策 | `https://screenhop.app/privacy` |
| 广告 | 否，不含广告 |
| 应用访问权限 | 所有功能无需特殊权限或登录即可使用 |
| 内容分级（IARC） | 类别选"实用工具/效率/通讯或其他"；暴力、色情、赌博、毒品等全部选"否"；用户之间能否互动或分享内容：否（投屏只在用户自己的设备之间，不经过网络服务）；是否分享位置：否 |
| 目标受众 | 13 岁以上（不面向儿童），避免受"家庭"政策约束 |
| 数据安全 | 不收集数据、不分享数据。传输加密：是（规则文件走 HTTPS）。删除数据请求：不适用（没有账号）。说明：读取配置文件时托管商能看到 IP，属于一般网络请求，不用于识别用户 |
| 新闻应用 / 政府应用 / 金融 / 健康 | 均为否 |
| 前台服务权限 | `mediaPlayback`：用于在后台保持投屏接收并播放用户投送的音视频 |

## 发布流程

1. 先发 **内部测试**，自己在电视上从 Play 安装一遍，确认安装、投屏都正常。
2. **注意**：2023 年 11 月之后注册的**个人**开发者账号，正式发布前必须先做**封闭测试：至少 12 名测试者、连续 14 天**。如果你的账号符合这个条件，请先找人加入封闭测试；组织账号或更早的个人账号不受此限。
3. 正式版本说明（首次发布）：
   - en-US：`First release: AirPlay screen mirroring, AirPlay video and music, DLNA casting from Android and Windows.`
   - zh-CN：`首个版本：支持 AirPlay 屏幕镜像、视频和音乐投送，以及安卓和 Windows 的 DLNA 投屏。`
4. 发布前把 GitHub 仓库改为公开（GPL 要求能获取源码，隐私政策里的 Issues 链接也需要能打开）。

## 已知问题（不影响上架）

- DLNA 的 HTTP 服务在播放期间偶尔（约 1–2%）出现连接被重置，控制端下一次轮询会恢复，原因待查。
- UxPlay 里的 AirPlay `pi`（配对标识）是固定值；同一个家里装两台 ScreenHop 时是否会让 iPhone 混淆，还没有验证。
