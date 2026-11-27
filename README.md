# Screenatic

Free, open-source (GPL-3.0) screen-mirroring receiver for **Google TV** and **Amazon Fire TV**
(Fire OS, and Vega OS in `vega/`). iPhone / iPad / Mac cast over AirPlay; Android phones, apps and
Windows ("Cast to Device") over DLNA. No paid tier and no time limit. Costs are to be covered by a
self-run sponsor slot on the idle home screen (never during casting, no ad SDK); store builds don't
show it yet.

## Status (2026-11-27)

| Feature | Google TV / Fire OS (`app`) | Fire TV Vega OS (`vega/`) |
|---|---|---|
| AirPlay mirroring with sound (1080p, H.264) | ✅ | ✅ |
| AirPlay video (YouTube, Safari, video apps hand over the URL) | ✅ | ✅ |
| Audio-only AirPlay (Music) with a Now Playing screen | ✅ | ✅ |
| DLNA renderer: video / music / photos | ✅ (tested with Windows "Cast to Device") | ✅ |
| 10 UI languages | ✅ | ✅ |
| Remote rules (`rules.json`, A/B buckets, forced update) | ✅ | ✅ |
| Idle sponsor slot with reach statistics | ✅ built, on in dev builds only | ✅ built, on in dev builds only |
| Rewarded ads for cast time | built with a placeholder ad, switched off | same |

**Released**

| Where | Version | State |
|---|---|---|
| Google Play (Android TV / Google TV) | 1.0.4 (5) | closed testing (see Install) |
| Amazon Appstore, Fire OS APK | 1.0.4 (5), replacing the submitted 1.0.3 (4) | submitted |
| Amazon Appstore, Vega vpkg | 1.0.4 (5), replacing 1.0.3 (4); adds the app icon | submitted |
| [GitHub Releases](https://github.com/mikeaifetel/screenatic/releases) | 1.0.4 APK; 1.0.3 APK + vpkg | sideload |

**Not done yet**

- Sponsor slot in store builds: privacy policy, sponsor policy page and store ad declarations
  first (see Sponsor slot), then `sponsorSlot: true` in `rules.json`.
- Windows screen mirroring and extended display (MS-MICE, Miracast over the LAN). Wi-Fi Direct
  Miracast is out of scope: apps can't use the TV's Wi-Fi Direct.
- H.265 / 4K mirroring on devices that can decode it.
- DLNA with Android phone senders: not tested yet.
- Google Cast is not planned (a receiver needs Google certification).

## Install

Google Play is in closed testing. To join:

1. Join the Google Group: <https://groups.google.com/g/screenatic>
2. With the same Google account, opt in: <https://play.google.com/apps/testing/com.screenatic.receiver>
3. Install: on a phone or computer, open <https://play.google.com/store/apps/details?id=com.screenatic.receiver> in a web browser (not the Play Store app),
   signed in with the same account, click Install and pick your TV. A test app usually doesn't show up in search on the TV.

Or sideload the APK / Vega vpkg from [Releases](https://github.com/mikeaifetel/screenatic/releases).
The Play and GitHub builds are signed differently, so uninstall one before installing the other. If Play says an incompatible version is installed, uninstall Screenatic on the TV in every user profile.

## Modules

| Module | What it does |
|---|---|
| `app` | Compose for TV UI, foreground `ReceiverService`, ExoPlayer for URL casting. Flavors `google` / `amazon`: store links and the rewarded-ad hook (`src/<flavor>/.../store/StoreModule.kt`; only a placeholder ad, no ad SDK). |
| `core` | `ReceiverEngine` interface, `SessionManager` (one sender at a time + cast-time metering), `CastTimeLedger`, remotely tunable `CastTimeRules` with A/B bucketing, ad interface. |
| `protocol-airplay` | AirPlay receiver. Kotlin `AirPlayEngine` + mDNS advertiser; native `libscreenatic_airplay.so` (JNI bridge, AMediaCodec decoder, UxPlay wrapper). |
| `protocol-dlna` | DLNA / UPnP MediaRenderer: hand-rolled SSDP, HTTP/SOAP and GENA eventing (no UPnP library). Video/music via ExoPlayer, photos via the image viewer. |
| `vega/` | Vega OS app (Fire TV Stick 4K Select and newer): React Native UI plus a native module with the same UxPlay core and patches, a C++ port of the DLNA renderer and the same cast-time rules. See `vega/README.md`. |

```
Phone/PC ──mDNS/SSDP──▶ AirPlayEngine / DlnaEngine ──state──▶ SessionManager ──▶ MainActivity
                                │ H.264 (native)                    │ meters CastTimeLedger
                                ▼                                   ▼
                       AMediaCodec → Surface              Grace banner (metering, off)
```

## Build

Open the folder in Android Studio (SDK path is in `local.properties`), or:

```bash
./gradlew :app:assembleGoogleDebug :app:assembleAmazonDebug
```

Without the prebuilt deps the native library builds a **stub** AirPlay server: the TV shows up on iPhones
but connecting does nothing. That lets UI, ads and the cast-time loop be tested first.
With the deps present, UxPlay's core is compiled in and the real AirPlay server runs (mirroring with
sound, URL casting, audio-only AirPlay).

### Enabling real AirPlay (UxPlay)

1. UxPlay is a git submodule pinned to **v1.73.7** (`scripts/fetch-uxplay.sh`; after cloning this repo run `git submodule update --init`).
2. `scripts/build-deps.sh` downloads (checksum-verified) and cross-compiles **OpenSSL 3.5.9 (libcrypto)** and
   **libplist 2.8.1** as static libs for `armeabi-v7a` / `arm64-v8a` into `protocol-airplay/third_party/prebuilt/<abi>/`
   (git-ignored; rerun after a fresh clone). When these exist, CMake builds UxPlay's core instead of the stub.
   After adding or removing them, delete `protocol-airplay/.cxx` so CMake reconfigures.
3. `src/main/cpp/compat/dns_sd.h` + `dns_sd_shim.c` (done): TXT-record helpers in Bonjour wire format and a no-op
   `DNSServiceRegister`, so UxPlay's `dnssd.c` builds the real TXT records that `AirPlayAdvertiser` publishes via NsdManager.
4. `airplay_server.cpp` (done): raop server + dnssd TXT records, video mirroring callbacks (H.264/H.265 → AMediaCodec,
   keyframe cache so decoding starts when the Surface appears), connect/disconnect, picture size, persistent ed25519 key
   in `noBackupFilesDir`. Verified with iPhones on Google TV and Fire TV.
5. Audio (done, `audio_player.cpp`): UxPlay upstream decodes through GStreamer; here AAC-ELD / AAC-LC go through
   AMediaCodec and ALAC through Apple's open-source decoder, into a small jitter buffer played with Oboe.

### UxPlay patches

UxPlay is compiled from a copy in the CMake build tree with `protocol-airplay/patches/*.patch` applied
(the submodule itself stays untouched). The configure step fails loudly if a patch no longer applies
after a UxPlay upgrade.

- `0001-direct-url-playback.patch`: UxPlay only accepts YouTube-style `…/master.m3u8` AirPlay-video
  playlists and rejects everything else (which also tears down mirroring). Safari and most apps send the
  real media URL, so it is handed straight to ExoPlayer instead.
- `0002-audio-resend-wait.patch`: wait at most ~24 packets for a resend instead of the whole 256-packet
  buffer, so a lost packet no longer silences mirrored audio for seconds.
- `0003-hls-local-host.patch`: configurable host in the rewritten YouTube HLS playlist URLs.
- `0004-audio-udp-trace.patch`: debug counters for the audio sockets and buffer.
- `0005-audio-skip-silence-gaps.patch`: skip a whole run of missing packets at once, not one per call.
- `0006-audio-setup-running-ports.patch`: a second audio SETUP without TEARDOWN (mirroring resumes after a
  video, Photos starts a video) answers with the running session's ports; before, audio went nowhere.

The Vega app applies the same patches.

### AirPlay video (URL casting)

Senders that support it (YouTube, Safari, many video apps) hand over the video instead of mirroring:
ExoPlayer plays it full screen at source quality, reports position/duration back for the sender's
progress bar (`/playback-info`), and follows seek / pause / resume / stop from the phone.

## DLNA renderer

`DlnaEngine` advertises a MediaRenderer (`urn:schemas-upnp-org:device:MediaRenderer:1`) over SSDP and serves
`/description.xml`, the three SCPDs and SOAP/GENA endpoints on port 49595 (any free port if taken).

- AVTransport: SetAVTransportURI / SetNextAVTransportURI / Play / Pause / Stop / Seek (REL_TIME, ABS_TIME),
  Get{Transport,Position,Media}Info, LastChange events. Playback failures report `ERROR_OCCURRED`.
- RenderingControl: Get/SetVolume (perceptual curve into ExoPlayer's volume), Get/SetMute, LastChange events.
- ConnectionManager: sink protocol info for the formats ExoPlayer and the image viewer handle.
- Media type comes from DIDL-Lite (`upnp:class` / `res@protocolInfo`), else the URL extension.

Quick check from a computer on the same Wi-Fi: any UPnP control point (e.g. BubbleUPnP on Android, or a
script doing M-SEARCH + SOAP) should list "Screenatic TV".

## Cast-time rules (ads for time)

All rules are remote-tunable and A/B-testable. Config lives in this repo at `docs/rules.json`
(served by Cloudflare Workers static assets, `wrangler.jsonc`: `https://screenatic.com/rules.json`); debug builds read
`docs/rules-dev.json`, so changes can be tried on dev builds first. The app refreshes it at start and
every 6 h, caches the last good copy, and falls back to the built-in defaults (free, no ads) offline.

Current policy (2026-11-26): `metering` stays off (no time limit). `sponsorSlot` turns on the sponsor
slot (below); it is on in `rules-dev.json` and off in `rules.json` until the store declarations and
privacy policy are updated. `idleAds` must stay `false`: builds up to 1.0.4 (Android) / 1.0.3 (Vega)
read it as a grey "Ad slot" placeholder.

| Field (JSON) | Default | Meaning |
|---|---|---|
| `metering` | false | Meter cast time and offer rewarded ads. Off = free, no ad UI at all |
| `welcomeGrantMin` | 30 | Free minutes on first launch |
| `rewardMin` | 60 | Minutes per rewarded ad |
| `maxBalanceMin` | 480 | Balance cap |
| `graceSec` | 120 | Casting continues this long after the balance hits zero |
| `dailyRewardCap` | 0 | Max rewarded ads per day (0 = unlimited) |
| `sponsorSlot` | false | Show the idle-screen sponsor slot (see Sponsor slot) |
| `idleAds` | false | Old switch, keep `false`: older builds show an "Ad slot" placeholder for it |

Top level: `version` (config revision), `minVersionCode` (builds below it show an "update" screen
with a store link and stop advertising), `defaults` (any field above) and an optional `experiment`
with weighted `variants`, each overriding fields. Devices are bucketed by hash(experiment id + device
id), so a device stays in its arm. Dev builds: the home-screen switch overrides `metering`.
The Vega app reads the same file the same way (vega/README.md), except that it is gated by its own
`minVegaBuild` instead of `minVersionCode`. While the "time is up" banner shows, the remote's OK opens the ad.

## Sponsor slot

The idle home screen's right half can show a house promo or a directly sold sponsor, with no ad SDK
(so no GPL conflict). Content comes from `docs/sponsors.json` (`https://screenatic.com/sponsors.json`;
debug builds read `sponsors-dev.json`): per item a kind (`house` → "From Screenatic", `sponsored` →
the localized "Sponsored" / 广告 label), localized title and text, an optional image, a URL drawn as a
QR code, optional languages / regions / excluded regions / UTC start-end dates and a rotation weight.
The format is documented in `app/.../sponsor/Sponsors.kt`. Both apps fetch the whole file (only while
`sponsorSlot` is on and the app is in front), filter it on the device by language, country and date, cache
it for 6 h and rotate items every 30 s. Nothing is sent to the server. The country (for `regions` /
`excludeRegions`, so sponsors can be sold per country) is the one Cloudflare derives from the
connection: `site/worker.js` runs only for the sponsor paths (`run_worker_first` in `wrangler.jsonc`),
adds `"country": "CA"` and serves the files uncached. Without it (an old cached file) the apps fall
back to the device's language-region setting.

**Reach statistics** (for sponsors): an impression is an item on screen for at least a second. The
apps count them on the device and send one batch per sponsor-file refresh (`POST /px`: file, app,
build, `{id: count}`; no device identifier). The QR code is `screenatic.com/c/{id}`: the worker counts
the scan and redirects to the item's `qr` URL (put UTM parameters there, so the sponsor sees the
scans too). The worker writes aggregate data points to Workers Analytics Engine (dataset
`screenatic_sponsor_events`: item, event, country, app, build, file, count; no IP is stored), which
keeps 3 months; `scripts/sponsor-report.sh` lists counts per day, item, event and country (needs a
Cloudflare API token with Account Analytics: Read). Free tier: 100k worker requests and 100k data
points a day. Store consoles' active-device counts per country complete the picture. The slot is neither focusable
nor clickable (Google TV forbids ads that open web pages); invalid items are dropped, and with nothing
left (or offline with no cache) the app's logo shows instead. Vega: `src/sponsors.ts`, `src/SponsorUi.tsx`.
QR codes: ZXing core (Apache-2.0) on Android, `qrcode-generator` (MIT) on Vega.

Text fits the card in every language when titles stay within 40 units and texts within 150 (a CJK /
full-width character counts 2): the card shows 2 title lines and 4 text lines, ellipsized, and the
apps cut anything past 80 / 300 characters. Run `python3 scripts/check-sponsors.py` before pushing a
change to `docs/sponsors*.json` (the site deploys on every push): it checks these limits, the format,
URLs, dates and images.

Before turning it on for store builds: Play "Contains ads" = Yes (house promos count too), privacy
policy text on sponsored content (the country is derived from the connection; anonymous impression
and scan counts; QR codes pass through screenatic.com), and a public sponsor policy. Test the worker locally with `npx wrangler@4 dev`. Plan and rules: `marketing/sponsor-slot.md`
(kept outside the repo).

**Gradual rollout** (e.g. turning metering on): keep the experiment `id` fixed, list the growing arm
first, and only raise its weight — devices already in it stay in it.

```json
{ "version": 2, "minVersionCode": 3,
  "experiment": { "id": "metering_rollout", "variants": [
    { "name": "metered", "weight": 10, "rules": { "metering": true } },
    { "name": "free",    "weight": 90 } ] } }
```

Only ship `metering: true` to builds that actually contain an ad source; use `minVersionCode` to
move older builds up first.

## Backend

None for now, on purpose:

- **Config**: static JSON on Cloudflare (above), deployed on every push to main.
- **Cast-time ledger**: on the device. Cheating only yields free cast time, so no server-side
  reward verification yet; add the ad network's SSV callback (small serverless function) if abuse matters.
- **Stats / crashes**: Google Play Console and Amazon developer console (installs, retention,
  Android vitals crashes/ANRs) — no analytics SDK in the app.
- **Sponsor reach**: the site's small worker (`site/worker.js`) counts impressions and QR scans in
  Workers Analytics Engine (see Sponsor slot). That is the only server-side code.
- **When A/B results matter**: comparing arms needs per-variant numbers the consoles don't give;
  the same worker could take `{variant, event}` counts (needs a privacy-policy update).

## Store release

Store graphics, TV screenshots, listing text (10 languages) and the Play Console checklist live in
`store/` (see `store/README.md`). The upload key is kept outside the repo in `~/.screenatic/`.

Store title: `Screenatic: Phone & PC Mirror` (Google Play caps titles at 30 characters;
"Screenatic: Screen Mirroring for Phone & PC" is 42, so use it on Amazon and in the short description).

## Licensing (read before adding ad SDKs)

Screenatic reuses UxPlay, which is **GPL-3.0**, so Screenatic is GPL-3.0 and its source must be published
with every release.

**Open issue:** ad SDKs such as Google IMA / AdMob and Amazon APS are proprietary.
Shipping them linked into a GPL-3.0 app conflicts with the GPL, and the UxPlay authors are the only ones
who could grant an exception. Options to decide (with a lawyer) before adding any ad SDK (rewarded ads are on hold):

- Keep the GPL AirPlay core in a **separate process/APK** talking over a socket, and keep the ad app non-GPL.
- Use ad delivery that needs no proprietary SDK (e.g. VAST over HTTP played in ExoPlayer, server-side ad decisioning).
- Accept the risk (what many GPL-based receivers on the stores do) – not recommended.

The skeleton keeps every ad SDK out of the build for now (`DevRewardedAd` placeholder)
so either path stays open.

Apple, AirPlay, iPhone, Mac are trademarks of Apple Inc.; Screenatic is not affiliated with Apple, Google or Amazon.
Do not use "AirPlay" in the app name or store title.

## Roadmap

Done so far is in Status. Next, roughly in order:

1. **1.0.5** (Android and Vega): sponsor slot live in store builds, with the privacy policy, sponsor
   policy page and store declarations ("Contains ads" on Play); first real items in `sponsors.json`.
   Vega: check that the AirPlay port it advertises is the one it actually holds.
2. **MS-MICE** (Windows mirroring and extended display over the LAN), Android / Fire OS first.
3. **H.265 / 4K** mirroring where the device decodes it.

On hold: rewarded ads for cast time (mechanism built, `metering` off) and a server-side cast-time
ledger. Only revisit if the no-time-limit decision changes, and see Licensing first.
