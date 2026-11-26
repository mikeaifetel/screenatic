# Screenatic

Free screen-mirroring receiver for **Google TV** and **Amazon Fire TV** (Fire OS, and Vega OS in `vega/`).
iPhone / iPad / Mac (AirPlay), Android phones and apps (DLNA) and Windows ("Cast to Device", DLNA) cast to
the TV; Windows screen mirroring (MS-MICE) is planned.
Free and open source, no paid tier, and casting is not time-limited. The plan is to cover costs with an ad on the idle
home screen (never during casting), once the ad-SDK licensing question below is settled; until then there are no ads.
A "watch an ad for more cast time" mechanism is built in (see Cast-time rules) but switched off.

Store title: `Screenatic: Phone & PC Mirror` (Google Play caps titles at 30 characters;
"Screenatic: Screen Mirroring for Phone & PC" is 42, so use it on Amazon and in the short description).

## Modules

| Module | What it does |
|---|---|
| `app` | Compose for TV UI, foreground `ReceiverService`, ExoPlayer for URL casting. Flavors `google` / `amazon` hold the ad SDKs (`src/<flavor>/.../store/StoreModule.kt`). |
| `core` | `ReceiverEngine` interface, `SessionManager` (one sender at a time + cast-time metering), `CastTimeLedger`, remotely tunable `CastTimeRules` with A/B bucketing, ad interface. |
| `protocol-airplay` | AirPlay receiver. Kotlin `AirPlayEngine` + mDNS advertiser; native `libscreenatic_airplay.so` (JNI bridge, AMediaCodec decoder, UxPlay wrapper). |
| `protocol-dlna` | DLNA / UPnP MediaRenderer: hand-rolled SSDP, HTTP/SOAP and GENA eventing (no UPnP library). Video/music via ExoPlayer, photos via the image viewer. |
| `vega/` | Vega OS app (Fire TV Stick 4K Select and newer): React Native UI plus a native module with the same UxPlay core and patches, a C++ port of the DLNA renderer and the same cast-time rules. See `vega/README.md`. |

```
Phone/PC ──mDNS/SSDP──▶ AirPlayEngine / DlnaEngine ──state──▶ SessionManager ──▶ MainActivity
                                │ H.264 (native)                    │ meters CastTimeLedger
                                ▼                                   ▼
                       AMediaCodec → Surface              Grace banner / rewarded ad
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

Current policy (2026-11-26): `metering` stays off (no time limit). `idleAds` will be switched on once a
real ad source passes the licensing check; until then the slot is only a placeholder.

| Field (JSON) | Default | Meaning |
|---|---|---|
| `metering` | false | Meter cast time and offer rewarded ads. Off = free, no ad UI at all |
| `welcomeGrantMin` | 30 | Free minutes on first launch |
| `rewardMin` | 60 | Minutes per rewarded ad |
| `maxBalanceMin` | 480 | Balance cap |
| `graceSec` | 120 | Casting continues this long after the balance hits zero |
| `dailyRewardCap` | 0 | Max rewarded ads per day (0 = unlimited) |
| `idleAds` | false | Show the idle-screen ad slot |

Top level: `version` (config revision), `minVersionCode` (builds below it show an "update" screen
with a store link and stop advertising), `defaults` (any field above) and an optional `experiment`
with weighted `variants`, each overriding fields. Devices are bucketed by hash(experiment id + device
id), so a device stays in its arm. Dev builds: the home-screen switch overrides `metering`.
The Vega app reads the same file the same way (vega/README.md), except that it is gated by its own
`minVegaBuild` instead of `minVersionCode`. While the "time is up" banner shows, the remote's OK opens the ad.

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
- **When A/B results matter**: comparing arms needs per-variant numbers the consoles don't give.
  Smallest option: an anonymous counter endpoint (e.g. Cloudflare Worker, free tier) receiving
  `{variant, event}`; needs a privacy-policy update.

## Beta testing (Google Play)

The Google Play version is in closed testing. To join:

1. Join the Google Group: <https://groups.google.com/g/screenatic>
2. With the same Google account, opt in: <https://play.google.com/apps/testing/com.screenatic.receiver>
3. Install: on a phone or computer, open <https://play.google.com/store/apps/details?id=com.screenatic.receiver> in a web browser (not the Play Store app),
   signed in with the same account, click Install and pick your TV. A test app usually doesn't show up in search on the TV.

Or sideload the APK / Vega vpkg from [Releases](https://github.com/mikeaifetel/screenatic/releases).
The Play and GitHub builds are signed differently, so uninstall one before installing the other. If Play says an incompatible version is installed, uninstall Screenatic on the TV in every user profile.

## Store release

Store graphics, TV screenshots, listing text (10 languages) and the Play Console checklist live in
`store/` (see `store/README.md`). The upload key is kept outside the repo in `~/.screenatic/`.

## Licensing (read before adding ad SDKs)

Screenatic reuses UxPlay, which is **GPL-3.0**, so Screenatic is GPL-3.0 and its source must be published
with every release.

**Open issue:** ad SDKs such as Google IMA / AdMob and Amazon APS are proprietary.
Shipping them linked into a GPL-3.0 app conflicts with the GPL, and the UxPlay authors are the only ones
who could grant an exception. Options to decide (with a lawyer) before shipping any ad (M2):

- Keep the GPL AirPlay core in a **separate process/APK** talking over a socket, and keep the ad app non-GPL.
- Use ad delivery that needs no proprietary SDK (e.g. VAST over HTTP played in ExoPlayer, server-side ad decisioning).
- Accept the risk (what many GPL-based receivers on the stores do) – not recommended.

The skeleton keeps every ad SDK out of the build for now (`DevRewardedAd` placeholder)
so either path stays open.

Apple, AirPlay, iPhone, Mac are trademarks of Apple Inc.; Screenatic is not affiliated with Apple, Google or Amazon.
Do not use "AirPlay" in the app name or store title.

## Roadmap

- **M0** stub runs on Fire TV & Google TV; UxPlay wired; iPhone mirroring at 1080p, latency measured.
- **M1** ~~AirPlay audio (AAC-ELD)~~ ✅, ~~URL casting (HLS / direct URL)~~ ✅, ~~ALAC audio-only AirPlay + Now Playing~~ ✅, ~~DLNA DMR (video / music / photos)~~ ✅ (tested with Windows "Cast to Device"; Android phone senders not yet).
- **M2** idle home-screen ad (`idleAds`), once the ad-SDK licensing question is settled (see Licensing). ~~Rules endpoint~~ ✅ (`rules.json`, shared by Android and Vega).
  On hold: rewarded ads for cast time and a server-side cast-time ledger (SSV). The mechanism is built (local ledger,
  placeholder ad) but `metering` stays off: casting is not time-limited. Only revisit if that decision changes.
- **M3** MS-MICE (Windows), H.265/4K on capable devices.
- **M4** Vega OS app (separate React Native codebase reusing the native core), on par with the Android app: AirPlay mirroring with sound at 1080p, AirPlay video (URL mode), audio-only AirPlay, DLNA (video / music / photos), cast-time rules and ad placeholder; leaves the cast lists while in the background. 1.0.3 uploaded to the Amazon Appstore. See `vega/README.md`.
