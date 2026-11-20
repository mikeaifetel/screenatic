# ScreenHop

Free screen-mirroring receiver for **Google TV** and **Amazon Fire TV**.
iPhone / iPad / Mac (AirPlay), Android phones and apps (DLNA), and Windows (MS-MICE, planned) cast to the TV.
Completely free, no paid tier: users watch short ads to earn cast time. Revenue = rewarded ads + idle-screen ads.

Store title: `ScreenHop: Phone & PC Mirror` (Google Play caps titles at 30 characters;
"ScreenHop: Screen Mirroring for Phone & PC" is 42, so use it on Amazon and in the short description).

## Modules

| Module | What it does |
|---|---|
| `app` | Compose for TV UI, foreground `ReceiverService`, ExoPlayer for URL casting. Flavors `google` / `amazon` hold the ad SDKs (`src/<flavor>/.../store/StoreModule.kt`). |
| `core` | `ReceiverEngine` interface, `SessionManager` (one sender at a time + cast-time metering), `CastTimeLedger`, remotely tunable `CastTimeRules` with A/B bucketing, ad interface. |
| `protocol-airplay` | AirPlay receiver. Kotlin `AirPlayEngine` + mDNS advertiser; native `libscreenhop_airplay.so` (JNI bridge, AMediaCodec decoder, UxPlay wrapper). |
| `protocol-dlna` | DLNA MediaRenderer (stub, M1). |

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
With the deps present, UxPlay's core is compiled in and the real AirPlay server runs (video mirroring;
audio and URL casting are M1).

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
   in `noBackupFilesDir`. Verified on an emulator: server starts, TXT records published via NsdManager.
   Not yet verified with a real iPhone on a real TV.
5. Audio: UxPlay decodes AAC-ELD/ALAC through GStreamer; on Android use AMediaCodec / fdk-aac + ALAC and play through AAudio.

## Cast-time rules (ads for time)

All rules are remote-tunable and A/B-testable; defaults live in `CastTimeRules`:

| Field (JSON) | Default | Meaning |
|---|---|---|
| `welcomeGrantMin` | 30 | Free minutes on first launch |
| `rewardMin` | 60 | Minutes per rewarded ad |
| `maxBalanceMin` | 480 | Balance cap |
| `graceSec` | 120 | Casting continues this long after the balance hits zero |
| `dailyRewardCap` | 0 | Max rewarded ads per day (0 = unlimited) |
| `idleAds` | true | Show the idle-screen ad slot |

Set `RULES_URL` in `app/build.gradle.kts` to a JSON endpoint (format in `CastTimeRulesRepository`). The JSON has
`defaults` plus an optional `experiment` with weighted `variants`, each overriding any field. Devices are bucketed
by hash(experiment id + device id), so a device stays in its arm; the assignment is cached, and the last good config
survives offline starts. Log `experimentId` / `variant` with every ad event to compare arms.

## Licensing (read before adding ad SDKs)

ScreenHop reuses UxPlay, which is **GPL-3.0**, so ScreenHop is GPL-3.0 and its source must be published
with every release.

**Open issue:** ad SDKs such as Google IMA / AdMob and Amazon APS are proprietary.
Shipping them linked into a GPL-3.0 app conflicts with the GPL, and the UxPlay authors are the only ones
who could grant an exception. Options to decide (with a lawyer) before M2:

- Keep the GPL AirPlay core in a **separate process/APK** talking over a socket, and keep the ad app non-GPL.
- Use ad delivery that needs no proprietary SDK (e.g. VAST over HTTP played in ExoPlayer, server-side ad decisioning).
- Accept the risk (what many GPL-based receivers on the stores do) – not recommended.

The skeleton keeps every ad SDK out of the build for now (`DevRewardedAd` placeholder)
so either path stays open.

Apple, AirPlay, iPhone, Mac are trademarks of Apple Inc.; ScreenHop is not affiliated with Apple, Google or Amazon.
Do not use "AirPlay" in the app name or store title.

## Roadmap

- **M0** stub runs on Fire TV & Google TV; UxPlay wired; iPhone mirroring at 1080p, latency measured.
- **M1** AirPlay audio + URL casting, DLNA DMR, embedded mDNS if NsdManager TXT records prove unreliable.
- **M2** rewarded ads, server-side cast-time ledger (SSV), rules endpoint + first A/B test.
- **M3** MS-MICE (Windows), H.265/4K on capable devices.
- **M4** Vega OS app (separate React Native codebase reusing the native core).
