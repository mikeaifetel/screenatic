#include "AirPlayTest.h"
#include "MssePlayer.h"
#include "Timeline.h"

#include "Fmp4Muxer.h"
#include "airplay_server.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}
#include <FLAC/stream_encoder.h>

#include "ALACBitUtilities.h"
#include "ALACDecoder.h"

#include <apmf/errors.h>
#include <apmf/process.h>
#include <apmf/ptr.h>
#include <apmf/iface/com/amazon/kepler/user_engagement/IKeplerUserEngagementModuleV2.h>
#include <apmf/iface/com/amazon/kepler/user_engagement/IUserEngagement.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace ProbeNativeTurboModule {

namespace {

// AudioSpecificConfig for AAC-ELD 44.1 kHz stereo, 480 samples per frame (UxPlay ct=8).
const uint8_t kAacEldConfig[] = {0xf8, 0xe8, 0x50, 0x00};
// AAC-LC 44.1 kHz stereo (audio-only AirPlay from some apps, ct=4).
const uint8_t kAacLcConfig[] = {0x12, 0x10};
// ALAC magic cookie ('alac' atom + ALACSpecificConfig): 352 spf, 16-bit, pb 40, mb 10, kb 14,
// 2 ch, maxRun 255, 44100 Hz. As in the Android build; AirPlay never sends one (ct=2: Music).
const uint8_t kAlacCookie[] = {
    0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
constexpr uint32_t kAlacFrames = 352;

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A writable directory that survives restarts, for the AirPlay identity key and the volume.
// Vega: /data is the app's own storage; /tmp is a tmpfs emptied on every launch, so a key kept
// there made the receiver a new device each time (the iPhone had to pair again, slowly).
std::string keyDir() {
  static const std::string dir = [] {
    std::vector<std::string> c = {"/data"};
    for (const char* env : {"HOME", "TMPDIR"})
      if (const char* d = getenv(env)) if (*d) c.push_back(d);
    c.push_back("/tmp");
    for (auto& d : c) {
      std::string probe = d + "/.screenatic_probe";
      if (FILE* f = fopen(probe.c_str(), "w")) {
        fclose(f);
        unlink(probe.c_str());
        return d;
      }
    }
    return std::string("/tmp");
  }();
  return dir;
}

// AirPlay volume curve (same as the Android AudioPlayer): -30..0 dB from the sender, -144 = mute.
float gainForDb(float db) {
  if (db <= -30.0f) return 0.0f;
  if (db >= 0.0f) return 1.0f;
  float frac = (30.0f + db) / 30.0f;
  float flat = -30.0f * (1.0f - frac);
  float tapered = 10.0f * std::log2(frac);
  return std::pow(10.0f, std::max(flat, tapered) / 20.0f);
}

// The receiver volume persists like on Android (senders are told it when they connect, so the
// iPhone slider continues where the TV was). First run: -15 dB, about half the slider; starting
// at 0 dB (100%) was startling at TV volume.
constexpr float kFirstRunVolumeDb = -15.0f;

std::string volumeFile() { return keyDir() + "/airplay_volume_db"; }

float loadVolumeDb() {
  FILE* f = fopen(volumeFile().c_str(), "r");
  float db = kFirstRunVolumeDb;
  if (f) {
    if (fscanf(f, "%f", &db) != 1) db = kFirstRunVolumeDb;
    fclose(f);
  }
  return db;
}

void saveVolumeDb(float db) {
  if (FILE* f = fopen(volumeFile().c_str(), "w")) {
    fprintf(f, "%.2f\n", db);
    fclose(f);
  }
}

std::string base64(const uint8_t* p, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = static_cast<uint32_t>(p[i]) << 16;
    if (i + 1 < n) v |= static_cast<uint32_t>(p[i + 1]) << 8;
    if (i + 2 < n) v |= p[i + 2];
    o += t[(v >> 18) & 63];
    o += t[(v >> 12) & 63];
    o += i + 1 < n ? t[(v >> 6) & 63] : '=';
    o += i + 2 < n ? t[v & 63] : '=';
  }
  return o;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

/**
 * Tells Vega the user is watching video while a mirror session runs, so the screensaver doesn't
 * cover a still phone screen (a mirror sends almost no frames while nothing moves on it).
 * AirPlay video (URL mode) plays in Vega's own player, which reports this by itself.
 */
class WatchingHint {
public:
  void set(bool on) {
    std::lock_guard<std::mutex> g(mu_);
    if (on == on_) return;
    try {
      if (!engagement_) {
        using apmf::iface::com::amazon::kepler::user_engagement::IKeplerUserEngagementModuleV2;
        auto module = apmf::GetProcessObject()
                          ->getComponent("/com.amazon.kepler.user_engagement.user_engagement_module")
                          .TryQueryInterface<IKeplerUserEngagementModuleV2>();
        if (!module) {
          Timeline::add("user engagement: module not available");
          return;
        }
        engagement_ = module->makeVideoPlaybackUserEngagement();
      }
      if (on) engagement_->start();
      else engagement_->stop();
      on_ = on;
      Timeline::add(on ? "user engagement: watching" : "user engagement: idle");
    } catch (const std::exception& e) {
      Timeline::add(std::string("user engagement failed: ") + e.what());
    } catch (...) {
      Timeline::add("user engagement failed");
    }
  }

private:
  std::mutex mu_;
  bool on_ = false;
  apmf::Ptr<apmf::iface::com::amazon::kepler::user_engagement::IUserEngagement> engagement_;
};

}  // namespace

struct AirPlayTest::Impl {
  Log log;
  Advertise advertise;
  Media media;
  Media audioMedia;
  std::mutex videoMu;
  Fmp4Muxer mux;
  // Debug capture of the session start (served by GET /dump.* on :7000).
  std::string dumpH264, dumpMp4, dumpPts;
  static constexpr size_t kDumpCap = 20 * 1024 * 1024;
  std::unique_ptr<screenatic::AirPlayServer> server;
  bool debug = false;
  WatchingHint watching;
  std::mutex eventMu;
  std::vector<std::string> events;  // JSON objects for pollEvents()

  void pushEvent(std::string json) {
    std::lock_guard<std::mutex> g(eventMu);
    if (events.size() < 64) events.push_back(std::move(json));
  }
  int port = 0;
  std::string state = "not started";

  // Video: counted only (stage 1 muxes it into fMP4).
  std::atomic<uint64_t> videoFrames{0}, videoBytes{0};
  /** Mirror H.264 access unit -> muxer -> JS (also fed by the :7100 debug ingest). */
  std::function<void(const uint8_t*, size_t, int64_t)> videoIn;
  MssePlayer msse;
  std::atomic<int> width{0}, height{0};
  std::atomic<bool> firstFrameSeen{false};
  std::atomic<bool> h265{false};

  // Audio: AAC-ELD (mirror) / AAC-LC / ALAC (audio-only) -> PCM -> FLAC (libFLAC), timed.
  std::mutex audioMu;
  int audioCt = 0;
  std::unique_ptr<ALACDecoder> alac;
  std::vector<int16_t> alacOut;
  AVCodecContext* aac = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  FLAC__StreamEncoder* flac = nullptr;
  std::vector<FLAC__int32> pcm;  // interleaved, what libFLAC takes
  FlacMp4Muxer amux;
  std::vector<uint8_t> flacHeader;  // "fLaC" + STREAMINFO, as libFLAC writes it at init
  std::vector<uint8_t> audioOut;    // fMP4 bytes waiting to leave the lock
  std::atomic<float> gain{1.0f};    // AirPlay volume
  uint64_t audioPackets = 0, decodeErrors = 0, pcmFrames = 0, flacBytes = 0;
  int64_t decodeUs = 0, encodeUs = 0;
  float levelPeak = 0;
  int64_t levelWindowUs = 0;
  std::string codecState = "idle";

  // libFLAC writes "fLaC" and the STREAMINFO block (samples == 0) during init, then one call per
  // encoded frame (samples > 0). Called with audioMu held.
  static FLAC__StreamEncoderWriteStatus onFlacWrite(const FLAC__StreamEncoder*, const FLAC__byte buffer[],
                                                    size_t bytes, uint32_t samples, uint32_t, void* self) {
    auto* d = static_cast<Impl*>(self);
    d->flacBytes += bytes;
    if (samples == 0) {
      d->flacHeader.insert(d->flacHeader.end(), buffer, buffer + bytes);
      const auto& h = d->flacHeader;
      if (h.size() >= 42 && memcmp(h.data(), "fLaC", 4) == 0 && (h[4] & 0x7f) == 0) {
        d->amux.setStreamInfo(h.data() + 8, 34);
      }
    } else {
      d->amux.addFrame(buffer, bytes, samples, d->audioOut);
      d->msse.pushAudio(buffer, bytes, samples);
    }
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }

  void closeAudioLocked() {
    if (flac) {
      FLAC__stream_encoder_finish(flac);
      FLAC__stream_encoder_delete(flac);
      flac = nullptr;
    }
    if (aac) avcodec_free_context(&aac);
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    alac.reset();
  }

  bool openAudioLocked(int ct) {
    closeAudioLocked();
    audioCt = ct;
    std::string what;
    if (ct == 2) {
      alac = std::make_unique<ALACDecoder>();
      if (alac->Init(const_cast<uint8_t*>(kAlacCookie), sizeof kAlacCookie) != 0) {
        alac.reset();
        codecState = "ALAC init failed";
        return false;
      }
      alacOut.assign(4096 * 2, 0);
      what = "ALAC";
    } else if (ct == 8 || ct == 4) {
      const uint8_t* cfg = ct == 8 ? kAacEldConfig : kAacLcConfig;
      const size_t cfgLen = ct == 8 ? sizeof kAacEldConfig : sizeof kAacLcConfig;
      const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
      if (!dec) { codecState = "no AAC decoder in libavcodec"; return false; }
      aac = avcodec_alloc_context3(dec);
      aac->extradata = static_cast<uint8_t*>(av_mallocz(cfgLen + AV_INPUT_BUFFER_PADDING_SIZE));
      memcpy(aac->extradata, cfg, cfgLen);
      aac->extradata_size = static_cast<int>(cfgLen);
      aac->sample_rate = 44100;
      av_channel_layout_default(&aac->ch_layout, 2);
      if (avcodec_open2(aac, dec, nullptr) < 0) { codecState = "avcodec_open2 failed"; return false; }
      pkt = av_packet_alloc();
      frame = av_frame_alloc();
      what = ct == 8 ? "AAC-ELD" : "AAC-LC";
    } else {
      codecState = "ct=" + std::to_string(ct) + " not supported";
      return false;
    }
    amux.reset();
    flacHeader.clear();

    flac = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(flac, 2);
    FLAC__stream_encoder_set_bits_per_sample(flac, 16);
    FLAC__stream_encoder_set_sample_rate(flac, 44100);
    FLAC__stream_encoder_set_compression_level(flac, 0);
    // One FLAC frame per AAC-ELD packet; the player's STREAMINFO says 480 (MssePlayer flacConfig),
    // so ALAC (352) and AAC-LC (1024) are re-blocked to 480 too.
    FLAC__stream_encoder_set_blocksize(flac, 480);
    FLAC__stream_encoder_set_streamable_subset(flac, false);
    if (FLAC__stream_encoder_init_stream(flac, onFlacWrite, nullptr, nullptr, nullptr, this) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
      codecState = "FLAC encoder init failed";
      return false;
    }
    codecState = what + " decoder + FLAC encoder ready";
    return true;
  }

  /** ALAC (16-bit interleaved) -> volume -> FLAC. */
  void decodeAlacLocked(const uint8_t* data, size_t size) {
    const float g = gain.load();
    int64_t t0 = nowUs();
    BitBuffer bits;
    BitBufferInit(&bits, const_cast<uint8_t*>(data), static_cast<uint32_t>(size));
    uint32_t n = 0;
    if (alac->Decode(&bits, reinterpret_cast<uint8_t*>(alacOut.data()), kAlacFrames, 2, &n) != 0) {
      ++decodeErrors;
      return;
    }
    pcm.resize(static_cast<size_t>(n) * 2);
    for (size_t i = 0; i < static_cast<size_t>(n) * 2; ++i) {
      const float x = alacOut[i] / 32768.0f;
      levelPeak = std::max(levelPeak, std::fabs(x));
      float v = x * g * 32767.0f;
      v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
      pcm[i] = static_cast<FLAC__int32>(v);
    }
    int64_t t1 = nowUs();
    decodeUs += t1 - t0;
    if (n > 0) FLAC__stream_encoder_process_interleaved(flac, pcm.data(), n);
    encodeUs += nowUs() - t1;
    pcmFrames += n;
  }

  void onAudio(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    {
      std::lock_guard<std::mutex> g(audioMu);
      decodeLocked(data, size);
      out.swap(audioOut);
    }
    // Outside audioMu: the sink takes the probe's lock, which status() holds while reading ours.
    if (!out.empty()) audioMedia(std::move(out));
  }

  void decodeLocked(const uint8_t* data, size_t size) {
    ++audioPackets;
    if (alac && flac) {
      decodeAlacLocked(data, size);
      return;
    }
    if (!aac || !flac) return;
    const float g = gain.load();
    int64_t t0 = nowUs();
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = static_cast<int>(size);
    if (avcodec_send_packet(aac, pkt) < 0) { ++decodeErrors; return; }
    while (avcodec_receive_frame(aac, frame) == 0) {
      int n = frame->nb_samples, ch = frame->ch_layout.nb_channels;
      pcm.resize(static_cast<size_t>(n) * 2);
      // FFmpeg's AAC decoder outputs planar float.
      for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
          const float* p = reinterpret_cast<const float*>(frame->extended_data[ch > 1 ? c : 0]);
          levelPeak = std::max(levelPeak, std::fabs(p[i]));
          float v = p[i] * g * 32767.0f;
          v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
          pcm[static_cast<size_t>(i) * 2 + c] = static_cast<FLAC__int32>(v);
        }
      }
      int64_t t1 = nowUs();
      decodeUs += t1 - t0;
      FLAC__stream_encoder_process_interleaved(flac, pcm.data(), static_cast<uint32_t>(n));
      t0 = nowUs();
      encodeUs += t0 - t1;
      pcmFrames += static_cast<uint64_t>(n);
    }
    // Debug: peak level of the sender's audio (before volume) and the volume, per 2 s.
    if (t0 - levelWindowUs >= 2000000) {
      char b[96];
      snprintf(b, sizeof b, "audio level: peak %.3f (%.0f dBFS), gain %.3f", levelPeak,
               levelPeak > 0 ? 20 * std::log10(levelPeak) : -200.0, g);
      Timeline::add(b);
      levelPeak = 0;
      levelWindowUs = t0;
    }
  }
};

AirPlayTest::AirPlayTest(Log log, Advertise advertise, Media media, Media audioMedia)
    : impl_(std::make_unique<Impl>()) {
  impl_->log = std::move(log);
  impl_->advertise = std::move(advertise);
  impl_->media = std::move(media);
  impl_->audioMedia = std::move(audioMedia);
}

AirPlayTest::~AirPlayTest() {
  if (impl_->server) impl_->server->stop();
  std::lock_guard<std::mutex> g(impl_->audioMu);
  impl_->closeAudioLocked();
}

std::string AirPlayTest::start(const std::string& deviceName, const std::string& deviceId,
                               const std::string& lanIp, bool debug) {
  Impl* d = impl_.get();
  d->debug = debug;
  if (d->server) return "airplay: already running on :" + std::to_string(d->port);

  screenatic::AirPlayOptions opt;
  opt.deviceName = deviceName;
  opt.deviceId = deviceId;
  opt.keyFile = keyDir() + "/airplay_ed25519.pem";
  opt.h265 = false;  // Vega MSE HEVC support not verified; mirror as H.264
  opt.fixedPortBase = 7200;  // OS-picked ports (>= 32768) were refused from the LAN on Vega
  opt.initialVolumeDb = loadVolumeDb();
  opt.hlsHost = lanIp;
  // 1080p: 720p (needed while the JS MSE buffer filled up) left text soft on a 1080p TV.
  // Native MSSE has no such buffer limit.
  opt.maxWidth = 1920;
  opt.maxHeight = 1080;
  opt.traceRequests = debug;  // timeline of the connection steps in GET /status
  d->gain = gainForDb(opt.initialVolumeDb);

  screenatic::AirPlayCallbacks cb;
  cb.onLog = [d](const std::string& m) {
    Timeline::add(m);
    // RTSP requests and per-2 s audio counters: timeline only
    if (m.compare(0, 8, "request ") != 0 && m.compare(0, 10, "audio udp:") != 0 && m.compare(0, 10, "audio rtp:") != 0) d->log(m);
  };
  cb.onConnected = [d](const std::string& name, screenatic::ClientKind k) {
    d->msse.setAudioOnly(k == screenatic::ClientKind::Audio);
    if (k == screenatic::ClientKind::Audio) {
      // Audio-only AirPlay (Music etc.): the JS shows what's playing.
      d->pushEvent("{\"type\":\"music\",\"state\":\"connected\",\"client\":\"" + jsonEscape(name) + "\"}");
      d->watching.set(true);
    }
    if (k == screenatic::ClientKind::Mirror) {
      std::lock_guard<std::mutex> g(d->videoMu);
      d->mux.reset();  // new session: next keyframe starts with a fresh init segment
      d->mux.setHoldLimit(64u << 20);  // see Fmp4Muxer::setHoldLimit (player freezes at ~100 MB)
      d->msse.newStream();
      d->dumpH264.clear();
      d->dumpMp4.clear();
      d->dumpPts.clear();
    }
    if (k == screenatic::ClientKind::Mirror) {
      d->firstFrameSeen = false;
      // Mirroring is back, so any AirPlay video (Safari fullscreen) is over, even without /stop:
      // leaving fullscreen sometimes sends only rate=0, and the paused URL player then kept the
      // decoder (mirror picture frozen for minutes, mirror audio losing packets).
      d->pushEvent("{\"type\":\"mirror\"}");
      d->watching.set(true);
    }
    Timeline::add(std::string("connected (") + (k == screenatic::ClientKind::Mirror ? "mirror)" : "audio)"));
    d->log(std::string("connected: ") + name + (k == screenatic::ClientKind::Mirror ? " (mirror)" : " (audio)"));
  };
  cb.onDisconnected = [d] {
    d->log("disconnected");
    d->watching.set(false);
    d->pushEvent("{\"type\":\"disconnected\"}");
  };
  cb.onPlayUrl = [d](const std::string& client, const std::string& url, int64_t startMs) {
    // Logged in full: which apps hand over a URL (and what kind) is what URL mode depends on.
    d->log("play url from " + client + ": " + url.substr(0, 160) + " @" + std::to_string(startMs) + " ms");
    d->pushEvent("{\"type\":\"play\",\"url\":\"" + jsonEscape(url) + "\",\"startMs\":" +
                 std::to_string(startMs) + ",\"gain\":" + std::to_string(d->gain.load()) + "}");
  };
  cb.onPlaybackCommand = [d](int cmd, double value) {
    d->log("playback command " + std::to_string(cmd) + " value " + std::to_string(value));
    d->pushEvent("{\"type\":\"cmd\",\"cmd\":" + std::to_string(cmd) + ",\"value\":" + std::to_string(value) + "}");
  };
  d->videoIn = [d](const uint8_t* data, size_t size, int64_t ptsUs) {
    d->msse.push(data, size, ptsUs);  // also while suspended: it keeps the frames since the last keyframe
    std::vector<uint8_t> out;
    {
      std::lock_guard<std::mutex> g(d->videoMu);
      d->mux.push(data, size, ptsUs, out);
      if (d->debug && d->dumpH264.size() + size < Impl::kDumpCap) {
        d->dumpH264.append(reinterpret_cast<const char*>(data), size);
        d->dumpMp4.append(reinterpret_cast<const char*>(out.data()), out.size());
        bool key = false;
        for (size_t i = 0; i + 4 < size; ++i)
          if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5) { key = true; break; }
        d->dumpPts += std::to_string(ptsUs) + " " + std::to_string(size) + " " + (key ? "K" : "-") + "\n";
      }
    }
    if (!out.empty()) d->media(std::move(out));
  };
  cb.onVideo = [d](const uint8_t* data, size_t size, int64_t ptsUs, bool h265) {
    d->videoFrames++;
    d->videoBytes += size;
    if (!d->firstFrameSeen.exchange(true)) Timeline::add("first video frame, " + std::to_string(size) + " bytes");
    d->h265 = h265;
    if (h265) return;  // not advertised; MSE path is H.264 only
    d->videoIn(data, size, ptsUs);
  };
  cb.onVideoSize = [d](int w, int h) {
    d->width = w;
    d->height = h;
    {
      std::lock_guard<std::mutex> g(d->videoMu);
      d->mux.setSize(w, h);
    }
    d->log("video size " + std::to_string(w) + "x" + std::to_string(h));
    d->pushEvent("{\"type\":\"size\",\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) + "}");
  };
  cb.onAudioFormat = [d](int ct) {
    std::lock_guard<std::mutex> g(d->audioMu);
    d->openAudioLocked(ct);
    d->log("audio ct=" + std::to_string(ct) + ": " + d->codecState);
  };
  cb.onAudio = [d](const uint8_t* data, size_t size) { d->onAudio(data, size); };
  cb.onAudioVolume = [d](float db) {
    d->gain = gainForDb(db);
    Timeline::add("volume " + std::to_string(db) + " dB -> gain " + std::to_string(d->gain.load()));
    saveVolumeDb(db);
    // AirPlay video (URL mode) plays through Vega's player, which applies it as element volume.
    d->pushEvent("{\"type\":\"volume\",\"gain\":" + std::to_string(d->gain.load()) + "}");
  };
  cb.onTrackInfo = [d](const std::string& title, const std::string& artist, const std::string& album) {
    d->pushEvent("{\"type\":\"track\",\"title\":\"" + jsonEscape(title) + "\",\"artist\":\"" + jsonEscape(artist) +
                 "\",\"album\":\"" + jsonEscape(album) + "\"}");
  };
  cb.onCoverArt = [d](const uint8_t* data, size_t size) {
    // JPEG or PNG as a data: URI for the JS Image (a few hundred KB at most).
    std::string mime = size > 3 && data[0] == 0x89 && data[1] == 'P' ? "image/png" : "image/jpeg";
    d->pushEvent("{\"type\":\"cover\",\"uri\":\"" +
                 (size > 0 ? "data:" + mime + ";base64," + base64(data, size) : std::string()) + "\"}");
  };
  cb.onTrackProgress = [d](double positionSec, double durationSec) {
    char b[128];
    snprintf(b, sizeof b, "{\"type\":\"progress\",\"position\":%.3f,\"duration\":%.3f}", positionSec, durationSec);
    d->pushEvent(b);
  };
  cb.onAudioPlaying = [d](bool playing) {
    d->pushEvent(std::string("{\"type\":\"music\",\"state\":\"") + (playing ? "playing" : "paused") + "\"}");
  };
  cb.onAudioStop = [d] {
    std::lock_guard<std::mutex> g(d->audioMu);
    d->closeAudioLocked();
  };

  d->server = std::make_unique<screenatic::AirPlayServer>(opt, cb);
  int port = 0;
  if (!d->server->start(port)) {
    d->state = "start failed (see log)";
    d->server.reset();
    return "airplay: " + d->state;
  }
  d->port = port;
  d->state = "listening on :" + std::to_string(port);
  d->log("UxPlay " + d->state + ", key " + opt.keyFile);
  d->advertise(port, d->server->airplayTxt(), d->server->raopTxt());
  return "airplay: " + d->state;
}

std::string AirPlayTest::dump(const std::string& kind) {
  std::lock_guard<std::mutex> g(impl_->videoMu);
  if (kind == "h264") return impl_->dumpH264;
  if (kind == "mp4") return impl_->dumpMp4;
  return impl_->dumpPts;
}

void AirPlayTest::debugPlay(const std::string& url) {
  impl_->log("debug play: " + url);
  impl_->pushEvent("{\"type\":\"play\",\"url\":\"" + jsonEscape(url) + "\",\"startMs\":0,\"gain\":" +
                   std::to_string(impl_->gain.load()) + "}");
}

std::string AirPlayTest::msseAttach(const std::string& windowHandle) {
  std::string r = impl_->msse.attach(windowHandle);
  impl_->log("msse attach " + windowHandle + ": " + r);
  return r;
}

std::string AirPlayTest::msseCommand(const std::string& query) {
  std::string r = impl_->msse.command(query);
  impl_->log("msse command " + query + ": " + r);
  return r;
}

void AirPlayTest::debugVideo(const uint8_t* data, size_t size, int64_t ptsUs, bool first) {
  if (first) {
    std::lock_guard<std::mutex> g(impl_->videoMu);
    impl_->mux.reset();
    impl_->mux.setHoldLimit(64u << 20);
    impl_->mux.setSize(1280, 720);
    impl_->msse.newStream();
  }
  impl_->videoFrames++;
  impl_->videoBytes += size;
  if (impl_->videoIn) impl_->videoIn(data, size, ptsUs);
}

void AirPlayTest::pushEvent(std::string json) { impl_->pushEvent(std::move(json)); }

std::string AirPlayTest::pollEvents() {
  std::vector<std::string> ev;
  {
    std::lock_guard<std::mutex> g(impl_->eventMu);
    ev.swap(impl_->events);
  }
  std::string out = "[";
  for (size_t i = 0; i < ev.size(); ++i) out += (i ? "," : "") + ev[i];
  return out + "]";
}

void AirPlayTest::setPlayback(double durationSec, double positionSec, double rate, bool ready,
                              bool buffering, bool finished) {
  if (!impl_->server) return;
  screenatic::PlaybackState s;
  s.durationSec = durationSec;
  s.positionSec = positionSec;
  s.rate = static_cast<float>(rate);
  s.ready = ready;
  s.buffering = buffering;
  s.finished = finished;
  impl_->server->setPlaybackState(s);
}

std::string AirPlayTest::statusJson() const {
  Impl* d = impl_.get();
  std::ostringstream o;
  uint64_t packets, errors, frames, bytes;
  int64_t dec, enc;
  std::string codec;
  {
    std::lock_guard<std::mutex> g(d->audioMu);
    packets = d->audioPackets; errors = d->decodeErrors; frames = d->pcmFrames; bytes = d->flacBytes;
    dec = d->decodeUs; enc = d->encodeUs; codec = d->codecState;
  }
  double audioSec = frames / 44100.0;
  // CPU share of one core spent on audio conversion, relative to real time.
  double cpu = audioSec > 0 ? (dec + enc) / 1e6 / audioSec * 100.0 : 0;
  uint64_t muxed, keys;
  uint32_t inits;
  double keyGap;
  std::vector<double> keyTimes;
  bool holding = false;
  Fmp4Muxer::NalStats ns;
  double sinceKey = -1;
  uint64_t holds = 0, heldFrames = 0;
  {
    std::lock_guard<std::mutex> g(d->videoMu);
    muxed = d->mux.frames();
    inits = d->mux.initSegments();
    keys = d->mux.keyframes();
    keyGap = d->mux.lastKeyIntervalSec();
    keyTimes = d->mux.keyTimes();
    holding = d->mux.holding();
    ns = d->mux.nalStats();
    sinceKey = d->mux.secSinceKey();
    holds = d->mux.holds();
    heldFrames = d->mux.heldFrames();
  }
  o << "{\"state\":\"" << d->state << "\",\"timeline\":" << Timeline::json() << ",\"video\":{\"frames\":" << d->videoFrames.load()
    << ",\"muxed\":" << muxed << ",\"inits\":" << inits << ",\"keyframes\":" << keys
    << ",\"keyGapSec\":" << keyGap << ",\"holding\":" << (holding ? "true" : "false")
    << ",\"nal\":{\"pictures\":" << ns.pictures << ",\"idr\":" << ns.idr << ",\"nonIdrI\":" << ns.nonIdrI
    << ",\"sei\":" << ns.sei << ",\"seiRecovery\":" << ns.seiRecovery << "}"
    << ",\"secSinceKey\":" << sinceKey
    << ",\"msse\":" << d->msse.statusJson()
    << ",\"holds\":" << holds << ",\"heldFrames\":" << heldFrames << ",\"keyTimes\":[";
  for (size_t i = 0; i < keyTimes.size(); ++i) o << (i ? "," : "") << keyTimes[i];
  o << "]"
    << ",\"bytes\":" << d->videoBytes.load() << ",\"width\":" << d->width.load()
    << ",\"height\":" << d->height.load() << ",\"h265\":" << (d->h265 ? "true" : "false")
    << "},\"audio\":{\"codec\":\"" << codec << "\",\"packets\":" << packets
    << ",\"decodeErrors\":" << errors << ",\"seconds\":" << audioSec << ",\"flacBytes\":" << bytes
    << ",\"decodeMs\":" << dec / 1000 << ",\"encodeMs\":" << enc / 1000 << ",\"cpuPct\":" << cpu
    << "}}";
  return o.str();
}

}  // namespace ProbeNativeTurboModule
