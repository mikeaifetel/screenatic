# 上架 Google Play 指南

## 文件一览

| 用途 | 文件 |
|---|---|
| 上传包（AAB） | `app/build/outputs/bundle/googleRelease/app-google-release.aab`（构建：`./gradlew :app:bundleGoogleRelease`） |
| 亚马逊 Fire OS（APK） | `app/build/outputs/apk/amazon/release/app-amazon-release.apk`（构建：`./gradlew :app:assembleAmazonRelease`） |
| 亚马逊 Vega OS（vpkg） | `vega/app/build/armv7-release/screenatic_armv7.vpkg`（另有 aarch64、x86_64；构建：`cd vega/native && npm run build:native:release`，再 `cd ../app && npm run build:release`） |
| 应用图标 512×512 | `store/graphics/icon-512.png` |
| 置顶大图 1024×500 | `store/graphics/feature-1024x500.png` |
| 电视横幅 1280×720 | `store/graphics/tv-banner-1280x720.png` |
| 电视截图 1920×1080 | `store/screenshots/en-*.png`（英文及其他语言），`store/screenshots/zh-*.png`（简体中文） |
| 商店文案（10 种语言） | `store/listing/<语言>.md`：标题、简短说明、完整说明 |
| 隐私政策 | `https://screenatic.com/privacy`（中文：`https://screenatic.com/zh/privacy`） |

图形素材的源文件是同目录下的 `.svg`，用 Chrome 无头模式按精确尺寸渲染。

**版本号**：Android 和 Vega 保持一致，每次上传前一起加。Android 在 `app/build.gradle.kts`（`versionCode`、`versionName`），Vega 在 `vega/app/manifest.toml`（`build-number` = versionCode，`version` = versionName）、`vega/app/package.json` 的 `version`、`vega/app/src/rules.ts` 的 `APP_BUILD`（= build-number）。当前 Android 1.0.4 / 5（只修了 Android 的待机常亮屏，Vega 不受影响），Vega 1.0.3 / 4；Vega 下次发版时直接跳到和 Android 相同的版本号。

## 上传密钥（务必备份）

- 位置：`~/.screenatic/upload-keystore.jks`，密码在 `~/.screenatic/keystore.properties`（只在这台电脑上，不在仓库里）。
- 请把这两个文件存进密码管理器或其他安全的备份位置。
- 使用 Play 应用签名：Google 保管真正的应用签名密钥，这把只是"上传密钥"。万一丢失，可以在 Play Console 申请重置，但需要几天时间。
- 构建时自动读取；换电脑时把这两个文件放到同样位置即可（或设置环境变量 `SCREENATIC_KEYSTORE_PROPERTIES`）。

## Play Console 操作步骤

1. **创建应用**：名称 `Screenatic: Phone & PC Mirror`，默认语言 English (United States)，类型"应用"，免费。
   包名是 `com.screenatic.receiver`，第一次上传后就**不能再改**。
2. **设置 → 高级设置 → 设备类型**：添加 **Android TV**（按提示同意电视应用的质量要求）。电视横幅和电视截图在这一步之后才会出现在商店素材里。
3. **主要商品详情**：按语言逐个添加（en-US、zh-CN、zh-TW、ja-JP、ko-KR、es-ES、pt-BR、fr-FR、de-DE、it-IT），从 `store/listing/` 复制文案。
   - 图标、置顶大图、电视横幅：所有语言共用。
   - 电视截图：简体中文用 `zh-*.png`，其余语言用 `en-*.png`。
   - 手机截图：Play Console 要求必填（至少 2 张），直接传同样的电视截图即可（16:9 符合尺寸要求）。手机用户看不到这个 App，所以这些截图实际只在电视端和网页版商店显示。
4. **应用类别**：工具（Tools）。联系邮箱：填一个你愿意公开的邮箱（会显示在商店页面）。网站：`https://screenatic.com`。

## 应用内容（政策问卷）参考答案

| 问卷 | 答案 |
|---|---|
| 隐私政策 | `https://screenatic.com/privacy` |
| 广告 | 否，不含广告（现在的版本确实没有广告。以后接入首页广告时，**发布那个版本之前**要改成"是"，隐私政策也要同步更新） |
| 应用访问权限 | 所有功能无需特殊权限或登录即可使用 |
| 内容分级（IARC） | 类别选"实用工具/效率/通讯或其他"；暴力、色情、赌博、毒品等全部选"否"；用户之间能否互动或分享内容：否（投屏只在用户自己的设备之间，不经过网络服务）；是否分享位置：否 |
| 目标受众 | 13 岁以上（不面向儿童），避免受"家庭"政策约束 |
| 数据安全 | 不收集数据、不分享数据。传输加密：是（规则文件走 HTTPS）。删除数据请求：不适用（没有账号）。说明：读取配置文件时托管商能看到 IP，属于一般网络请求，不用于识别用户 |
| 新闻应用 / 政府应用 / 金融 / 健康 | 均为否 |
| 前台服务权限 | 上传 AAB 后才会出现。类型选**媒体播放**。说明：`Screenatic is a TV screen-mirroring receiver. The media-playback foreground service keeps the AirPlay and DLNA receiver running and plays the audio and video that the user casts from their own phone or computer. It starts when the user opens the app and stops when the user exits it.` 演示视频：把 `store/fgs-demo.mp4`（18 秒，打开 App → 投音乐 → 投照片）上传为 YouTube 不公开视频，填链接 |

## 发布流程

1. 先发 **内部测试**，自己在电视上从 Play 安装一遍，确认安装、投屏都正常。
2. **注意**：2023 年 11 月之后注册的**个人**开发者账号，正式发布前必须先做**封闭测试：至少 12 名测试者、连续 14 天**。如果你的账号符合这个条件，请先找人加入封闭测试；组织账号或更早的个人账号不受此限。
3. 正式版本说明（首次发布）：
   - en-US：`First release: AirPlay screen mirroring, AirPlay video and music, DLNA casting from Android and Windows.`
   - zh-CN：`首个版本：支持 AirPlay 屏幕镜像、视频和音乐投送，以及安卓和 Windows 的 DLNA 投屏。`
4. GitHub 仓库 `mikeaifetel/screenatic` 已公开（GPL 要求能获取源码，隐私政策里的 Issues 链接也需要能打开）。

## 改名遗留：删除旧的 ScreenHop 草稿应用

旧应用的包名 `app.screenhop.receiver` 已经上传过 AAB，不能改。新应用建好后，在旧应用的 **设置 → 高级设置 → 应用可用性**（或应用列表里该应用的菜单）找"删除应用"。只有从未发布过的应用才能删除。

## 已知问题（不影响上架）

- DLNA 的 HTTP 服务在播放期间偶尔（约 1–2%）出现连接被重置，控制端下一次轮询会恢复，原因待查。
- UxPlay 里的 AirPlay `pi`（配对标识）是固定值；同一个家里装两台 Screenatic 时是否会让 iPhone 混淆，还没有验证。

## 推广和 12 人测试

可以把这两个问题放在一起解决：**先在 Play 之外找到第一批真实用户，再请这些人帮忙完成封闭测试**。

**1. 先在 Play 之外发布**

- **GitHub Releases 放 APK。** Google TV 和 Fire TV 用户很习惯用 Downloader 之类的工具安装 APK。README 里写上"免费、开源的 AirPlay + DLNA 接收器"，并给出安装步骤（不写"无广告"，见下）。
- **F-Droid / IzzyOnDroid。** 暂缓：它们不收带广告或统计 SDK 的应用，而我们计划以后在首页放广告。
- **亚马逊 Fire TV 应用商店。** 旧款 Fire TV 运行的 Fire OS 本身就是安卓，可以直接装我们的 APK。这个商店**没有 12 人测试的要求**，Fire TV 用户也很多。手机版亚马逊应用商店已经关了，但 Fire TV 的商店还在。我们没有用 Google Play 服务，大概率不需要改代码，不过要真机验证一下。

**2. 去用户聚集的地方发帖**

同类产品（AirScreen 等）大多闭源，很多要付费。"免费、开源、投屏不限时长"就是卖点。**不要说"无广告"**：已经定了以后在待机首页放广告（2026-11-26），统一口径见 `marketing/posts.md`。适合在这些社区发：
- Reddit：r/AndroidTV、r/googletv、r/fireTV、r/opensource（先看各版的自我推广规则）
- XDA 论坛的 Android TV 版块，Hacker News 的 "Show HN"
- AlternativeTo 上，把它加为 AirScreen / AirPlay 接收器的替代品
- AFTVnews 专门报道 Fire TV 应用，可以给他们发邮件投稿
- 中文：V2EX、少数派

**3. 用这批用户完成封闭测试**

封闭测试只接受名单里的人：邮件列表，或者 Google 群组。想让陌生人自己加入，就用 **Google 群组当作公开报名表**（这是 12 人测试最常见的做法）：

1. 用你的 Google 账号打开 groups.google.com → 创建群组。**已建好**：`screenatic@googlegroups.com`（<https://groups.google.com/g/screenatic>）。
2. 群组设置：**谁可以搜索群组**选"网络上的任何人"，**谁可以加入群组**选"任何人都可以加入"。帖子和成员列表都设为仅管理员可见，避免暴露测试者的邮箱，也免得被当成讨论区。
3. Play Console → 测试和发布 → 封闭测试 → 测试人员 → 选 **Google 群组**，填 `screenatic@googlegroups.com`，保存。
4. 招募帖里给两个链接，并说明顺序：
   - 先点群组链接加入：<https://groups.google.com/g/screenatic>；
   - 再用**同一个 Google 账号**打开封闭测试的加入链接，点"成为测试人员"：<https://play.google.com/apps/testing/com.screenatic.receiver>（网页）；
   - 然后在手机或电脑的**浏览器**里（不要用 Play 商店 App）用同一个帐号打开 <https://play.google.com/store/apps/details?id=com.screenatic.receiver>，点"安装"，选电视远程安装。测试版在电视上一般搜不到，这是测试者最容易卡住的一步。
   - Play 版和 GitHub 上的 APK 签名不同，不能互相覆盖，换装前要先卸载另一个。之前侧载过的话，要在电视的每个用户档案里卸载，否则 Play 会提示"已安装了不兼容的版本"。

在 Reddit 上直接发帖："招募免费开源 AirPlay 接收器的测试用户"，贴出上面两个链接。看到帖子的人本来就有电视棒，是真实测试者，比请朋友帮忙靠谱得多。凑 12 个人加入、坚持 14 天，这样就可行了。

两个补充：
- **测试者不一定要有电视棒。** 规则要求的是 12 人"加入测试并保持 14 天"。网上有不少开发者互助群（Reddit 的 r/AndroidClosedTesting、r/TestersCommunity），大家互相加入对方的测试。不过申请正式版时，Google 会问测试者的参与情况，最好有几个真实使用的人。我不确定 Google 是否要求测试者真的装过 App，所以真实用户越多越稳妥。
- **组织类开发者账号不需要满足这条要求。** 但它需要邓白氏编码（D-U-N-S），而且要另外注册一个账号，对小项目来说不划算，只是知道有这个选项即可。

---

**建议的顺序**：把 GitHub Release 做好（APK 加上带截图的 README）→ 发到 Reddit 和 XDA → 在同一个帖子里招募封闭测试 → 14 天后申请正式版。同时可以考虑上架亚马逊 Fire TV 应用商店，那边没有测试门槛。

## 亚马逊 Fire TV 应用商店素材

`store/amazon/` 下是按亚马逊要求整理好的文件（都没有透明通道）：

| 上架页面的位置 | 文件 |
|---|---|
| App icon（必填，1280×720 PNG，无透明） | `amazon/app-icon-1280x720.png`（源文件 `graphics/firetv-icon-1280x720.svg`：比 Google 横幅缩小，内容在中间 882×448 安全区内，避免被下载/进度角标挡住） |
| Background image（必填，1920×1080，无透明） | `amazon/background-1920x1080.jpg`（源文件 `graphics/background-1920x1080.svg`；主体在右侧，不含文字和 logo，符合要求） |
| Screenshots（必填，1920×1080，3～10 张） | `amazon/en-*.jpg`；简体中文 `amazon/zh-*.jpg` 目前只有 2 张，不够 3 张时中文页也用英文截图 |
| 512×512 图标、1024×500 宣传图（如果页面要求） | `graphics/icon-512.png`、`graphics/feature-1024x500.png` |
| 文案 | `listing/<语言>.md`，与 Google Play 相同 |

"Images and videos" 这一块的三项必填（图标、背景图、至少 3 张截图）都传齐后才显示完成。Fire TV 的商品详情不需要视频。
