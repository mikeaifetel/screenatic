#include "AirPlayTest.h"

#include "airplay_server.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}
#include <FLAC/stream_encoder.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace ProbeNativeTurboModule {

namespace {

// AudioSpecificConfig for AAC-ELD 44.1 kHz stereo, 480 samples per frame (UxPlay ct=8).
const uint8_t kAacEldConfig[] = {0xf8, 0xe8, 0x50, 0x00};

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A writable directory for the persistent AirPlay identity key.
std::string keyDir() {
  for (const char* env : {"HOME", "TMPDIR"}) {
    const char* d = getenv(env);
    if (d && *d && access(d, W_OK) == 0) return d;
  }
  return "/tmp";
}

}  // namespace

struct AirPlayTest::Impl {
  Log log;
  Advertise advertise;
  std::unique_ptr<screenatic::AirPlayServer> server;
  int port = 0;
  std::string state = "not started";

  // Video: counted only (stage 1 muxes it into fMP4).
  std::atomic<uint64_t> videoFrames{0}, videoBytes{0};
  std::atomic<int> width{0}, height{0};
  std::atomic<bool> h265{false};

  // Audio: AAC-ELD -> PCM (FFmpeg) -> FLAC (libFLAC), timed.
  std::mutex audioMu;
  int audioCt = 0;
  AVCodecContext* aac = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  FLAC__StreamEncoder* flac = nullptr;
  std::vector<FLAC__int32> pcm;  // interleaved, what libFLAC takes
  uint64_t audioPackets = 0, decodeErrors = 0, pcmFrames = 0, flacBytes = 0;
  int64_t decodeUs = 0, encodeUs = 0;
  std::string codecState = "idle";

  static FLAC__StreamEncoderWriteStatus onFlacWrite(const FLAC__StreamEncoder*, const FLAC__byte[],
                                                    size_t bytes, uint32_t, uint32_t, void* self) {
    static_cast<Impl*>(self)->flacBytes += bytes;
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
  }

  bool openAudioLocked(int ct) {
    closeAudioLocked();
    audioCt = ct;
    if (ct != 8) {
      codecState = "ct=" + std::to_string(ct) + " (only AAC-ELD is decoded in this test)";
      return false;
    }
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!dec) { codecState = "no AAC decoder in libavcodec"; return false; }
    aac = avcodec_alloc_context3(dec);
    aac->extradata = static_cast<uint8_t*>(av_mallocz(sizeof kAacEldConfig + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(aac->extradata, kAacEldConfig, sizeof kAacEldConfig);
    aac->extradata_size = sizeof kAacEldConfig;
    aac->sample_rate = 44100;
    av_channel_layout_default(&aac->ch_layout, 2);
    if (avcodec_open2(aac, dec, nullptr) < 0) { codecState = "avcodec_open2 failed"; return false; }
    pkt = av_packet_alloc();
    frame = av_frame_alloc();

    flac = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(flac, 2);
    FLAC__stream_encoder_set_bits_per_sample(flac, 16);
    FLAC__stream_encoder_set_sample_rate(flac, 44100);
    FLAC__stream_encoder_set_compression_level(flac, 0);
    FLAC__stream_encoder_set_blocksize(flac, 480);  // one FLAC frame per AAC-ELD packet
    FLAC__stream_encoder_set_streamable_subset(flac, false);
    if (FLAC__stream_encoder_init_stream(flac, onFlacWrite, nullptr, nullptr, nullptr, this) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
      codecState = "FLAC encoder init failed";
      return false;
    }
    codecState = "AAC-ELD decoder + FLAC encoder ready";
    return true;
  }

  void onAudio(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> g(audioMu);
    ++audioPackets;
    if (!aac || !flac) return;
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
          float v = p[i] * 32767.0f;
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
  }
};

AirPlayTest::AirPlayTest(Log log, Advertise advertise) : impl_(std::make_unique<Impl>()) {
  impl_->log = std::move(log);
  impl_->advertise = std::move(advertise);
}

AirPlayTest::~AirPlayTest() {
  if (impl_->server) impl_->server->stop();
  std::lock_guard<std::mutex> g(impl_->audioMu);
  impl_->closeAudioLocked();
}

std::string AirPlayTest::start(const std::string& deviceName, const std::string& deviceId) {
  Impl* d = impl_.get();
  if (d->server) return "airplay: already running on :" + std::to_string(d->port);

  screenatic::AirPlayOptions opt;
  opt.deviceName = deviceName;
  opt.deviceId = deviceId;
  opt.keyFile = keyDir() + "/airplay_ed25519.pem";
  opt.h265 = false;  // Vega MSE HEVC support not verified; mirror as H.264
  opt.fixedPortBase = 7200;  // OS-picked ports (>= 32768) were refused from the LAN on Vega

  screenatic::AirPlayCallbacks cb;
  cb.onLog = [d](const std::string& m) { d->log(m); };
  cb.onConnected = [d](const std::string& name, screenatic::ClientKind k) {
    d->log(std::string("connected: ") + name + (k == screenatic::ClientKind::Mirror ? " (mirror)" : " (audio)"));
  };
  cb.onDisconnected = [d] { d->log("disconnected"); };
  cb.onVideo = [d](const uint8_t*, size_t size, int64_t, bool h265) {
    d->videoFrames++;
    d->videoBytes += size;
    d->h265 = h265;
  };
  cb.onVideoSize = [d](int w, int h) {
    d->width = w;
    d->height = h;
    d->log("video size " + std::to_string(w) + "x" + std::to_string(h));
  };
  cb.onAudioFormat = [d](int ct) {
    std::lock_guard<std::mutex> g(d->audioMu);
    d->openAudioLocked(ct);
    d->log("audio ct=" + std::to_string(ct) + ": " + d->codecState);
  };
  cb.onAudio = [d](const uint8_t* data, size_t size) { d->onAudio(data, size); };
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
  d->advertise(port, d->server->airplayTxt());
  return "airplay: " + d->state;
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
  o << "{\"state\":\"" << d->state << "\",\"video\":{\"frames\":" << d->videoFrames.load()
    << ",\"bytes\":" << d->videoBytes.load() << ",\"width\":" << d->width.load()
    << ",\"height\":" << d->height.load() << ",\"h265\":" << (d->h265 ? "true" : "false")
    << "},\"audio\":{\"codec\":\"" << codec << "\",\"packets\":" << packets
    << ",\"decodeErrors\":" << errors << ",\"seconds\":" << audioSec << ",\"flacBytes\":" << bytes
    << ",\"decodeMs\":" << dec / 1000 << ",\"encodeMs\":" << enc / 1000 << ",\"cpuPct\":" << cpu
    << "}}";
  return o.str();
}

}  // namespace ProbeNativeTurboModule
