#include "MssePlayer.h"

#include <apmf/apmf_base.h>
#include <apmf/errors.h>
#include <apmf/process.h>
#include <apmf/ptr.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayController.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGraphicsModule.h>
#include <apmf/iface/com/amazon/kepler/graphics/IMediaSurface.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/react/IReactModule.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/react/IUIManager.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/view/IKeplerMediaSurfaceProvider.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/view/ISurfaceView.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/view/ISurfaceViewListener.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/view/IView.h>
#include <apmf/iface/com/amazon/kepler/uitoolkit/view/IViewModule.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IAudioParams.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IConfig.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IConfigBuilder.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IEventInfo.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IHtmlMediaElement.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IHtmlMediaElement2.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IHtmlMediaElementEventListener.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IHtmlMediaElementInitStatusListener.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IHtmlMediaElementDeInitStatusListener.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IMediaError.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IMediaSample.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IMediaSampleSource.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IMediaSampleSourceTrack.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IVideoParams.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/IW3CMedia.h>
#include <apmf/iface/com/amazon/kepler/w3cmedia/VideoPlaybackQuality.h>

#include <FLAC/stream_encoder.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>

namespace ProbeNativeTurboModule {

namespace w3c = apmf::iface::com::amazon::kepler::w3cmedia;
namespace gfx = apmf::iface::com::amazon::kepler::graphics;
namespace uiview = apmf::iface::com::amazon::kepler::uitoolkit::view;
namespace uireact = apmf::iface::com::amazon::kepler::uitoolkit::react;
using apmf::Ptr;

namespace {

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o;
}

/** NAL units of an Annex-B buffer (start codes stripped). */
std::vector<std::pair<const uint8_t*, size_t>> splitNals(const uint8_t* p, size_t n) {
  std::vector<std::pair<const uint8_t*, size_t>> out;
  size_t i = 0, start = SIZE_MAX;
  while (i + 3 <= n) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && p[end - 1] == 0) --end;  // 4-byte start code / trailing zeros
        out.emplace_back(p + start, end - start);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < n) out.emplace_back(p + start, n - start);
  return out;
}

void u32be(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

/** AVCDecoderConfigurationRecord (ISO/IEC 14496-15) for one SPS and one PPS. */
std::vector<uint8_t> avcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> v{1, sps[1], sps[2], sps[3], 0xff, 0xe1};
  v.push_back(sps.size() >> 8); v.push_back(sps.size() & 0xff);
  v.insert(v.end(), sps.begin(), sps.end());
  v.push_back(1);
  v.push_back(pps.size() >> 8); v.push_back(pps.size() & 0xff);
  v.insert(v.end(), pps.begin(), pps.end());
  return v;
}

/** MSSE takes the elementary stream type: "video/mp4; codecs=avc1..." (the MSE form) makes the
 *  player backend fail ("MPB is not initialized", error 4); video/h264 starts the decoder. */
/**
 * Tell the KeplerVideoSurfaceView (call on the JS thread) that a player bound its surface, as the system's JS player does
 * (w3cmedia::W3CMediaViewRegistry::get()->notifySurfaceBound(handle, surfaceId) in
 * libkeplerscript-w3cmedia-viewobserver, already loaded in this process). Until then the view
 * keeps the UI opaque over the video: the picture rendered but only showed as the app closed.
 *
 * The library is built against libstdc++, this module against libc++, so the std::string
 * argument is laid out by hand (std::__cxx11::basic_string on 32-bit ARM: data pointer, length,
 * then 16 bytes of inline buffer or the capacity), passed by invisible reference as for any
 * non-trivial by-value argument.
 */
[[maybe_unused]] std::string notifyViewSurfaceBound(const std::string& handle, int surfaceId) {
  void* lib = dlopen("libkeplerscript-w3cmedia-viewobserver.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) lib = dlopen("libkeplerscript-w3cmedia-viewobserver.so.1", RTLD_NOW);
  if (!lib) return std::string("dlopen failed: ") + dlerror();
  auto get = reinterpret_cast<void* (*)()>(dlsym(lib, "_ZN8w3cmedia20W3CMediaViewRegistry3getEv"));
  struct GnuString {
    const char* p;
    uint32_t len;
    union {
      char buf[16];
      uint32_t cap;
    };
  };
  auto notify = reinterpret_cast<void (*)(void*, GnuString*, int)>(dlsym(
      lib,
      "_ZN8w3cmedia20W3CMediaViewRegistry18notifySurfaceBoundENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi"));
  if (!get || !notify) return "symbols not found";
  GnuString str{};
  str.len = static_cast<uint32_t>(handle.size());
  if (handle.size() < 16) {
    memcpy(str.buf, handle.data(), handle.size());
    str.buf[handle.size()] = 0;
    str.p = str.buf;
  } else {
    // Heap form; allocated with the global operator new, so the callee may take it over (move)
    // and free it. Otherwise it is leaked: a few bytes per surface.
    char* heap = static_cast<char*>(::operator new(handle.size() + 1));
    memcpy(heap, handle.data(), handle.size() + 1);
    str.p = heap;
    str.cap = str.len;
  }
  notify(get(), &str, surfaceId);
  return "notified surface " + std::to_string(surfaceId);
}

/** FLAC STREAMINFO for AirPlayTest's encoder: 480-sample blocks, 44.1 kHz, stereo, 16 bit,
 *  unknown frame sizes, length and MD5 (a live stream). withHeader: "fLaC" + metadata block
 *  header first (the form Android's csd-0 and GStreamer's streamheader use). */
std::vector<uint8_t> flacConfig(bool withHeader) {
  std::vector<uint8_t> v;
  if (withHeader) v = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};  // last block, STREAMINFO, 34 bytes
  v.insert(v.end(), {0x01, 0xe0, 0x01, 0xe0, 0, 0, 0, 0, 0, 0});  // block sizes, frame sizes
  const uint64_t x = (uint64_t{44100} << 44) | (uint64_t{1} << 41) | (uint64_t{15} << 36);
  for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8)));
  v.insert(v.end(), 16, 0);  // MD5 unknown
  return v;
}

std::string mimeFor(const std::vector<uint8_t>&) { return "video/h264"; }

}  // namespace

struct MssePlayer::Impl : std::enable_shared_from_this<MssePlayer::Impl> {
  // Recursive: UI callbacks (surface created) may arrive synchronously inside attach().
  mutable std::recursive_mutex mu;
  std::string state = "idle";
  std::string lastError;
  std::deque<std::string> events;  // recent player events, newest last
  std::atomic<bool> initialized{false};
  bool trackStarted = false;
  bool waitKey = true;
  int64_t firstPtsUs = -1;
  int64_t lastPtsMs = -1;
  int64_t wallStartUs = -1;   // steady clock at timeline 0
  int cushionMs = 100;        // queued before play(); latency target is this + 60 ms
  int64_t pullbackMs = 0;     // taken off arrival time to win back latency after a stall
  int64_t latencyMs = -1;     // arrival time minus play position (sampled while frames flow)
  int64_t sampleArrivalMs = -1;
  double lastPosSec = -1;     // play position, read in push without the lock
  bool suspended = false;     // decoder handed to the URL (Safari/YouTube) player
  std::vector<std::vector<uint8_t>> gop;  // frames since the last keyframe (see pushFrame)
  size_t gopBytes = 0;
  bool gopValid = false;
  mutable std::mutex cacheMu;
  mutable std::string lastStatus;
  uint64_t pullbacks = 0;
  bool started = false;       // play() issued (after the start-up cushion)
  int64_t cushionFrom = -1;   // timeline position when the cushion started
  uint64_t queued = 0, skipped = 0, queueErrors = 0, keyframes = 0;
  double queueMsMax = 0;
  std::vector<uint8_t> sps, pps;
  std::string mime;
  int width = 0, height = 0;
  // Sample format, switchable at runtime for format tests (command()).
  std::string mimeOverride;
  // Annex-B (start codes) + avcC is what Vega's decoder shows: length-prefixed samples were
  // accepted but never produced a picture.
  bool annexB = true;
  bool sendConfig = true;   // avcC as codec specific config data
  std::string handle;       // JS surface (window handle)
  // Audio track (runtime-switchable like the video format: amime=, aconfig=0|1|2, audio=0|1).
  bool audioOn = true;
  std::string audioMime = "audio/x-flac";  // "audio/flac" was accepted but stayed silent
  int audioConfig = 2;      // 0 none, 1 STREAMINFO, 2 "fLaC" + header + STREAMINFO
  Ptr<w3c::IMediaSampleSourceTrack> audioTrack;
  double audioNextMs = -1;  // timestamp of the next audio frame (contiguous)
  uint64_t audioQueued = 0, audioErrors = 0, audioReanchors = 0, silenceFrames = 0;
  double lastRealAudioMs = -1e9;  // timeline time of the last frame from the sender
  // Silence: with an audio track the player waits for audio data before it shows anything, and
  // an iPhone sends no audio while nothing plays sound. Own encoder, same format.
  FLAC__StreamEncoder* silenceEnc = nullptr;
  std::vector<std::vector<uint8_t>> silenceOut;
  static FLAC__StreamEncoderWriteStatus onSilence(const FLAC__StreamEncoder*, const FLAC__byte buf[], size_t n,
                                                  uint32_t samples, uint32_t, void* self) {
    if (samples > 0) static_cast<Impl*>(self)->silenceOut.emplace_back(buf, buf + n);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }
  /** Queues one FLAC frame at the audio timeline position; called with the lock held, returns
   *  with it held (the player call itself runs unlocked). */
  void queueAudioLocked(std::unique_lock<std::recursive_mutex>& g, std::vector<uint8_t> frame, uint32_t samples);
  /** Fills the audio track with silence up to timeline time untilMs. */
  void fillSilenceLocked(std::unique_lock<std::recursive_mutex>& g, double untilMs);
  ~Impl() {
    if (silenceEnc) {
      FLAC__stream_encoder_finish(silenceEnc);
      FLAC__stream_encoder_delete(silenceEnc);
    }
  }
  // makeMediaSample keeps a pointer to our bytes for the sample's lifetime; the player holds at
  // most a few seconds, so keep the last ~20 s of samples alive.
  std::deque<std::shared_ptr<std::vector<uint8_t>>> keepAlive;

  Ptr<w3c::IW3CMedia> media;
  Ptr<w3c::IHtmlMediaElement> element;
  Ptr<w3c::IMediaSampleSource> source;
  Ptr<w3c::IMediaSampleSourceTrack> track;
  Ptr<gfx::IGraphicsModule> graphics;
  Ptr<gfx::IDisplayController> display;
  Ptr<gfx::IMediaSurface> surface;
  Ptr<uiview::ISurfaceView> ownView;  // media surface view we add under the JS surface view
  Ptr<apmf::iface::com::amazon::apmf::ISubscription> initSub, eventSub, deinitSub;

  void note(const std::string& e) {
    events.push_back(e);
    while (events.size() > 12) events.pop_front();
  }
  void fail(const std::string& where, const std::string& what) {
    lastError = where + ": " + what;
    note("ERROR " + lastError);
  }

  bool startTrackLocked();
  void onInitializedLocked();
  /** Binds the pending surface once the element is initialized. */
  void bindSurfaceLocked();
};

namespace {

struct InitListener : apmf::ApmfBase<InitListener, w3c::IHtmlMediaElementInitStatusListener> {
  explicit InitListener(std::weak_ptr<MssePlayer::Impl> i) : impl(std::move(i)) {}
  void onInitialized() {
    if (auto d = impl.lock()) {
      std::lock_guard<std::recursive_mutex> g(d->mu);
      d->initialized = true;
      d->state = "initialized, waiting for a keyframe";
      d->note("initialized");
      d->onInitializedLocked();
    }
  }
  std::weak_ptr<MssePlayer::Impl> impl;
};

struct SurfaceViewListener : apmf::ApmfBase<SurfaceViewListener, uiview::ISurfaceViewListener> {
  explicit SurfaceViewListener(std::weak_ptr<MssePlayer::Impl> i) : impl(std::move(i)) {}
  void take(apmf::detail::View<uiview::ISurfaceView> v, const char* what) {
    auto d = impl.lock();
    if (!d) return;
    std::lock_guard<std::recursive_mutex> g(d->mu);
    try {
      auto p = v.TryQueryInterface<uiview::IKeplerMediaSurfaceProvider>();
      auto ms = p ? p->getMediaSurface() : nullptr;
      d->note(std::string("own surface view ") + what + (ms ? ": has surface" : ": no surface"));
      if (ms) {
        if (d->surface && d->element && d->initialized) d->element->clearVideoSurface(d->surface);
        d->surface = ms;
        d->bindSurfaceLocked();
      }
    } catch (std::exception const& e) {
      d->fail(std::string("surface view ") + what, e.what());
    }
  }
  void onSurfaceCreated(apmf::detail::View<uiview::ISurfaceView> v) { take(v, "created"); }
  void onSurfaceUpdated(apmf::detail::View<uiview::ISurfaceView> v) { take(v, "updated"); }
  void onSurfaceDestroyed(apmf::detail::View<uiview::ISurfaceView>) {}
  std::weak_ptr<MssePlayer::Impl> impl;
};

/** Depth-first search for a view that already provides a media surface. */
Ptr<gfx::IMediaSurface> findMediaSurface(apmf::detail::View<uiview::IView> v, int depth, std::string& trail) {
  if (auto p = v.TryQueryInterface<uiview::IKeplerMediaSurfaceProvider>()) {
    trail += "P";
    if (auto ms = p->getMediaSurface()) return ms;
  }
  if (depth <= 0) return nullptr;
  auto kids = v->getChildren();
  trail += "(" + std::to_string(kids.size());
  for (auto& k : kids) {
    if (auto ms = findMediaSurface(k, depth - 1, trail)) return ms;
  }
  trail += ")";
  return nullptr;
}

struct DeinitListener : apmf::ApmfBase<DeinitListener, w3c::IHtmlMediaElementDeInitStatusListener> {
  void onDeinitialized() {}
};

struct EventListener : apmf::ApmfBase<EventListener, w3c::IHtmlMediaElementEventListener> {
  explicit EventListener(std::weak_ptr<MssePlayer::Impl> i) : impl(std::move(i)) {}
  void onEvent(apmf::detail::StringView const name, w3c::EventSource const src,
               apmf::detail::View<w3c::IEventInfo> const) {
    if (auto d = impl.lock()) {
      std::string n(name.data(), name.size());
      // timeupdate/progress fire constantly; keep the interesting ones.
      if (n == "timeupdate" || n == "progress") return;
      std::string e = n + "/" + std::to_string(static_cast<int>(src));
      Ptr<w3c::IHtmlMediaElement> element;
      {
        std::lock_guard<std::recursive_mutex> g(d->mu);
        element = d->element;
      }
      if (n == "error" && element) {  // player call without the lock (see push)
        try {
          if (auto err = element->getError()) {
            auto m = err->getMessage();
            e += " code " + std::to_string(static_cast<int>(err->getCode())) + " " + std::string(m.data(), m.size());
          }
        } catch (...) {
        }
      }
      std::lock_guard<std::recursive_mutex> g(d->mu);
      d->note(e);
    }
  }
  std::weak_ptr<MssePlayer::Impl> impl;
};

}  // namespace

MssePlayer::MssePlayer() : impl_(std::make_shared<Impl>()) {}
MssePlayer::~MssePlayer() = default;

bool MssePlayer::active() const {
  std::lock_guard<std::recursive_mutex> g(impl_->mu);
  return impl_->element != nullptr;
}

std::string MssePlayer::attach(const std::string& windowHandle) {
  auto d = impl_;
  std::lock_guard<std::recursive_mutex> g(d->mu);
  const int64_t handle = std::strtoll(windowHandle.c_str(), nullptr, 10);
  d->handle = windowHandle;
  std::string step = "get process";
  try {
    auto proc = apmf::GetProcessObject();
    if (!d->media) {
      step = "get w3cmedia component";
      for (const char* name : {"/com.amazon.kepler.w3cmedia", "/com.amazon.kepler.w3cmedia_2"}) {
        try {
          auto c = proc->tryGetComponent(name);
          if (!c) {
            d->note(std::string(name) + ": not found");
            continue;
          }
          d->media = c.TryQueryInterface<w3c::IW3CMedia>();
          d->note(std::string(name) + (d->media ? ": ok" : ": no IW3CMedia"));
          if (d->media) break;
        } catch (std::exception const& e) {
          d->note(std::string(name) + ": " + e.what());
        }
      }
      if (!d->media) throw std::runtime_error("no W3C media component");
    }
    if (!d->element) {
      step = "make element";
      auto config = d->media->makeConfigBuilder()->playerMode(w3c::PlayerMode::MSSE)->build();
      d->element = d->media->makeHtmlMediaElement(config);
      step = "initialize";
      d->initSub = d->element->initializeAsync(Ptr<InitListener>::Make(std::weak_ptr<Impl>(d)));
      d->state = "initializing";
    }
    step = "graphics component";
    if (!d->graphics)
      d->graphics = proc->getComponent("/com.amazon.kepler.graphics").QueryInterface<gfx::IGraphicsModule>();
    step = "display controller";
    if (!d->display) d->display = d->graphics->createDisplayControllerForSystemClients(nullptr);
    // windowHandle is "<viewTag>": the KeplerVideoSurfaceView's React view tag. Its own media
    // surface (IKeplerMediaSurfaceProvider) is the one the UI shows; a surface created separately
    // for the same window rendered underneath the opaque UI (seen only as the app closed).
    (void)handle;
    step = "ui manager";
    auto react = proc->tryGetComponent("/com.amazon.kepler.uitoolkit.react");
    if (!react) throw std::runtime_error("no uitoolkit.react component");
    auto ui = react.QueryInterface<uireact::IReactModule>()->getUIManager();
    step = "view from tag";
    auto view = ui->tryGetViewFromTag(static_cast<uint32_t>(std::strtoul(windowHandle.c_str(), nullptr, 10)));
    if (!view) throw std::runtime_error("no view for tag " + windowHandle);
    step = "find surface";
    std::string trail;
    auto found = findMediaSurface(view, 3, trail);
    d->note("view tree " + trail + (found ? " -> surface" : " -> none"));
    if (found) {
      if (d->surface && d->element && d->initialized) d->element->clearVideoSurface(d->surface);
      d->surface = found;
    } else if (!d->ownView) {
      // Official native route (uitoolkit SurfaceView guide): a media surface view of our own,
      // added under the JS one and sized like it; its surface arrives in onSurfaceCreated.
      step = "make media surface view";
      auto vm = proc->getComponent("/com.amazon.kepler.uitoolkit.view").QueryInterface<uiview::IViewModule>();
      d->ownView = vm->makeMediaSurfaceView(Ptr<SurfaceViewListener>::Make(std::weak_ptr<Impl>(d)));
      auto size = view->getSize();
      d->ownView->setSize(size);
      d->ownView->setOffset({0, 0});
      step = "add child";
      view->addChild(d->ownView);
      d->note("own surface view " + std::to_string(static_cast<int>(size.width)) + "x" +
              std::to_string(static_cast<int>(size.height)));
      if (auto p = d->ownView.TryQueryInterface<uiview::IKeplerMediaSurfaceProvider>()) {
        if (auto ms = p->getMediaSurface()) d->surface = ms;
      }
    }
    if (!d->surface) return "ok: waiting for the surface view";
    d->note("surface " + windowHandle);
    // On the JS thread (attach is called from JS), like the system player: the view reacts by
    // changing the UI, which crashed the app when notified from a media callback thread.
    // notifyViewSurfaceBound crashed the app at start (3 times); off until understood.
    d->bindSurfaceLocked();  // now if initialized, else from onInitialized
    return "ok: " + d->state;
  } catch (std::exception const& e) {
    d->fail(step, e.what());
  } catch (...) {
    d->fail(step, "unknown exception");
  }
  return d->lastError;
}

void MssePlayer::Impl::onInitializedLocked() {
  // The element accepts other calls only after initialization.
  try {
    eventSub = element->addEventListener(Ptr<EventListener>::Make(std::weak_ptr<Impl>(shared_from_this())));
  } catch (std::exception const& e) {
    fail("add event listener", e.what());
  }
  bindSurfaceLocked();
}

void MssePlayer::Impl::bindSurfaceLocked() {
  if (!initialized || !surface || !element) return;
  try {
    element->setVideoSurface(surface);
    note("surface bound");
  } catch (std::exception const& e) {
    fail("set video surface", e.what());
  }
}

bool MssePlayer::Impl::startTrackLocked() {
  std::string step = "video params";
  try {
    if (!mimeOverride.empty()) mime = mimeOverride;
    auto params = media->makeVideoParams(mime);
    w3c::Resolution r{};
    r.videoWidth = width > 0 ? width : 1280;
    r.videoHeight = height > 0 ? height : 720;
    params->setVideoResolution(r);
    params->setVideoFrameRate(30);
    auto config = avcC(sps, pps);
    if (sendConfig)
      params->setCodecSpecificConfigData(apmf::detail::ArrayView<uint8_t const>(config.data(), config.size()));
    step = "sample source";
    source = media->makeMediaSampleSource(element);
    // Order matters (found on the device):
    // 1. add the track first: setMediaSampleSource builds the MSSE pipeline and connects only the
    //    tracks already added ("Track has not been initialized with an adapter yet" otherwise,
    //    and every sample is dropped);
    // 2. setMediaSampleSource replaces the element's pipeline, so bind the surface again after it
    //    (bound before, it stayed with the old pipeline: black screen).
    step = "video track";
    track = media->makeMediaSampleSourceTrackForVideo(params);
    step = "add track";
    source->addTrack(track);
    // Audio as a second track of the same player: one clock for both, so they stay in sync,
    // and the whole mirror (picture and sound) can be handed over to URL playback at once.
    audioTrack = nullptr;
    if (audioOn) {
      try {
        step = "audio track";
        auto ap = media->makeAudioParams(audioMime);
        ap->setAudioChannelCount(2);
        ap->setAudioSampleRate(44100);
        if (audioConfig > 0) {
          auto cfg = flacConfig(audioConfig == 2);
          ap->setCodecSpecificConfigData(apmf::detail::ArrayView<uint8_t const>(cfg.data(), cfg.size()));
        }
        audioTrack = media->makeMediaSampleSourceTrackForAudio(ap);
        source->addTrack(audioTrack);
        note("audio track " + audioMime + " config " + std::to_string(audioConfig));
      } catch (std::exception const& e) {
        audioTrack = nullptr;
        fail("audio track (video only)", e.what());
      }
    }
    step = "set sample source";
    element->setMediaSampleSource(source);
    step = "rebind surface";
    if (surface) element->setVideoSurface(surface);
    step = "start track";
    track->start();
    if (audioTrack) audioTrack->start();
    audioNextMs = -1;
    // play() only once cushionMs is queued (push): live frames arrive just in time, and without a
    // cushion every network hiccup stalls the player (waiting/playing all the time).
    started = false;
    cushionFrom = -1;
    trackStarted = true;
    state = "playing " + mime;
    note("track started " + mime);
    return true;
  } catch (std::exception const& e) {
    fail(step, e.what());
  } catch (...) {
    fail(step, "unknown exception");
  }
  return false;
}

namespace {
std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      o += s[i] == '+' ? ' ' : s[i];
    }
  }
  return o;
}
std::string param(const std::string& q, const std::string& key) {
  size_t p = q.find(key + "=");
  while (p != std::string::npos && p > 0 && q[p - 1] != '&' && q[p - 1] != '?') p = q.find(key + "=", p + 1);
  if (p == std::string::npos) return "";
  size_t s = p + key.size() + 1, e = q.find('&', s);
  return urlDecode(q.substr(s, e == std::string::npos ? std::string::npos : e - s));
}
}  // namespace

std::string MssePlayer::command(const std::string& query) {
  auto d = impl_;
  std::string handle;
  Ptr<w3c::IHtmlMediaElement> old;
  bool oldInitialized = false;
  bool suspend = false;
  {
    std::lock_guard<std::recursive_mutex> g(d->mu);
    if (query.find("mime=") != std::string::npos) d->mimeOverride = param(query, "mime");
    if (query.find("annexb=") != std::string::npos) d->annexB = param(query, "annexb") == "1";
    if (query.find("config=") != std::string::npos) d->sendConfig = param(query, "config") != "0";
    if (query.find("cushion=") != std::string::npos) d->cushionMs = std::atoi(param(query, "cushion").c_str());
    // suspend=1: release the decoder for the URL (Safari/YouTube) player; suspend=0: take it back.
    if (query.find("amime=") != std::string::npos) d->audioMime = param(query, "amime");
    if (query.find("aconfig=") != std::string::npos) d->audioConfig = std::atoi(param(query, "aconfig").c_str());
    if (query.find("audio=") != std::string::npos) d->audioOn = param(query, "audio") == "1";
    if (query.find("suspend=") != std::string::npos) d->suspended = param(query, "suspend") == "1";
    // Recreate the element: a track's format is fixed once added. Torn down below, unlocked.
    old = d->element;
    oldInitialized = d->initialized;
    d->eventSub = nullptr;
    d->initSub = nullptr;
    d->track = nullptr;
    d->audioTrack = nullptr;
    d->audioNextMs = -1;
    d->audioQueued = d->audioErrors = d->audioReanchors = d->silenceFrames = 0;
    d->lastRealAudioMs = -1e9;
    d->source = nullptr;
    d->element = nullptr;
    d->initialized = false;
    d->trackStarted = false;
    d->waitKey = true;
    d->lastPtsMs = -1;
    d->wallStartUs = -1;
    d->pullbackMs = 0;
    d->latencyMs = -1;
    d->sampleArrivalMs = -1;
    d->started = false;
    d->queued = d->skipped = d->queueErrors = d->keyframes = 0;
    d->queueMsMax = 0;
    d->lastError.clear();
    d->events.clear();
    d->note("format: mime=" + (d->mimeOverride.empty() ? std::string("auto") : d->mimeOverride) +
            (d->annexB ? " annexb" : " avcc") + (d->sendConfig ? " +config" : " -config"));
    handle = d->handle;
    suspend = d->suspended;
    if (suspend) d->state = "suspended (URL playback)";
  }
  if (old && oldInitialized) {
    try {
      old->pause();
      auto sub = old->deinitializeAsync(Ptr<DeinitListener>::Make());
      std::lock_guard<std::recursive_mutex> g(d->mu);
      d->deinitSub = sub;
    } catch (...) {
    }
  }
  if (suspend) return "suspended";
  std::this_thread::sleep_for(std::chrono::milliseconds(250));  // let the old pipeline go (800 ms made leaving Safari fullscreen ~2 s black)
  return handle.empty() ? "no surface yet" : attach(handle);
}

void MssePlayer::Impl::queueAudioLocked(std::unique_lock<std::recursive_mutex>& g, std::vector<uint8_t> frame,
                                         uint32_t samples) {
  const double durMs = samples * 1000.0 / 44100.0;
  const int64_t pts = static_cast<int64_t>(audioNextMs);
  audioNextMs += durMs;
  auto buf = std::make_shared<std::vector<uint8_t>>(std::move(frame));
  auto t = audioTrack;
  auto m = media;
  try {
    auto smp = m->makeMediaSample(apmf::detail::ArrayView<uint8_t const>(buf->data(), buf->size()));
    smp->setPresentationTimestamp(pts);
    smp->setDecodingTimestamp(pts);
    smp->setDuration(static_cast<int64_t>(durMs + 0.5));
    smp->setSyncFrame();  // every FLAC frame decodes on its own
    keepAlive.push_back(buf);
    while (keepAlive.size() > 2400) keepAlive.pop_front();
    g.unlock();  // no player call with the lock held (see push)
    t->queueMediaSample(smp);
    g.lock();
    ++audioQueued;
  } catch (std::exception const& e) {
    if (!g.owns_lock()) g.lock();
    ++audioErrors;
    fail("audio queue", e.what());
  }
}

void MssePlayer::Impl::fillSilenceLocked(std::unique_lock<std::recursive_mutex>& g, double untilMs) {
  if (!silenceEnc) {
    silenceEnc = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(silenceEnc, 2);
    FLAC__stream_encoder_set_bits_per_sample(silenceEnc, 16);
    FLAC__stream_encoder_set_sample_rate(silenceEnc, 44100);
    FLAC__stream_encoder_set_compression_level(silenceEnc, 0);
    FLAC__stream_encoder_set_blocksize(silenceEnc, 480);
    FLAC__stream_encoder_set_streamable_subset(silenceEnc, false);
    if (FLAC__stream_encoder_init_stream(silenceEnc, onSilence, nullptr, nullptr, nullptr, this) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
      FLAC__stream_encoder_delete(silenceEnc);
      silenceEnc = nullptr;
      fail("silence encoder", "init failed");
      return;
    }
  }
  static const std::vector<FLAC__int32> zeros(480 * 2, 0);
  int guard = 0;
  while (audioNextMs < untilMs && ++guard < 100) {
    silenceOut.clear();
    FLAC__stream_encoder_process_interleaved(silenceEnc, zeros.data(), 480);
    for (auto& f : silenceOut) {
      queueAudioLocked(g, std::move(f), 480);
      ++silenceFrames;
    }
    if (silenceOut.empty()) break;
  }
}

void MssePlayer::pushAudio(const uint8_t* frame, size_t size, uint32_t samples) {
  auto d = impl_;
  std::unique_lock<std::recursive_mutex> g(d->mu);
  if (d->suspended || !d->trackStarted || !d->audioTrack || d->wallStartUs < 0) return;
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  const double arrivalMs = (nowUs - d->wallStartUs) / 1000.0 - d->pullbackMs;
  // Contiguous timestamps from the sample count (arrival times jitter and would leave gaps and
  // overlaps); re-anchored to arrival time, on the video's timeline, after a gap or drift. Ahead
  // of arrival by up to 200 ms is fine (silence filled in front of it).
  if (d->audioNextMs < 0 || arrivalMs - d->audioNextMs > 200 || d->audioNextMs - arrivalMs > 400) {
    if (d->audioNextMs >= 0) ++d->audioReanchors;
    d->audioNextMs = arrivalMs;
  }
  d->lastRealAudioMs = arrivalMs;
  d->queueAudioLocked(g, std::vector<uint8_t>(frame, frame + size), samples);
}

void MssePlayer::newStream() {
  std::lock_guard<std::recursive_mutex> g(impl_->mu);
  impl_->waitKey = true;
  impl_->gop.clear();  // a new session starts with its own keyframe
  impl_->gopBytes = 0;
  impl_->gopValid = false;
  impl_->note("new stream");
}

void MssePlayer::push(const uint8_t* data, size_t size, int64_t ptsUs) {
  (void)ptsUs;  // timeline is arrival time (see pushFrame)
  pushFrame(data, size, false);
}

void MssePlayer::pushFrame(const uint8_t* data, size_t size, bool fromCache) {
  auto d = impl_;
  std::unique_lock<std::recursive_mutex> g(d->mu);
  // Keep every frame since the last keyframe (up to 40 MB). An iPhone sends a keyframe only when
  // a session starts and rarely after: returning from Safari fullscreen, the new session's only
  // keyframe arrived while the player was still being recreated, and the picture stayed black.
  // A player that starts without a keyframe replays this group first.
  if (!fromCache) {
    bool isKey = false;
    for (auto [p, n] : splitNals(data, size))
      if (n > 0 && (p[0] & 0x1f) == 5) isKey = true;
    if (isKey) {
      d->gop.clear();
      d->gopBytes = 0;
      d->gopValid = true;
    }
    if (d->gopValid) {
      d->gop.emplace_back(data, data + size);
      d->gopBytes += size;
      if (d->gopBytes > (40u << 20)) {
        d->gop.clear();
        d->gopBytes = 0;
        d->gopValid = false;
      }
    }
    if (!d->suspended && d->element && d->initialized && d->waitKey && !isKey && d->gopValid && d->gop.size() > 1) {
      auto frames = d->gop;  // includes this frame, last
      d->note("replaying " + std::to_string(frames.size()) + " frames since the last keyframe");
      g.unlock();
      for (auto& f : frames) pushFrame(f.data(), f.size(), true);
      return;
    }
  }
  if (d->suspended || !d->element || !d->initialized) return;

  auto sample = std::make_shared<std::vector<uint8_t>>();
  sample->reserve(size + 64);
  bool key = false;
  for (auto [p, n] : splitNals(data, size)) {
    if (n == 0) continue;
    const int type = p[0] & 0x1f;
    if (type == 7) d->sps.assign(p, p + n);
    if (type == 8) d->pps.assign(p, p + n);
    if (type == 9) continue;  // access unit delimiter
    if (type == 5) key = true;
    if (d->annexB)
      u32be(*sample, 1);  // 00 00 00 01 start code
    else
      u32be(*sample, static_cast<uint32_t>(n));  // AVCC: 4-byte length prefix, as in MP4
    sample->insert(sample->end(), p, p + n);
  }
  if (sample->empty()) return;
  if (d->waitKey) {
    if (!key || d->sps.size() < 4 || d->pps.empty()) {
      ++d->skipped;
      return;
    }
    d->waitKey = false;
  }
  if (!d->trackStarted) {
    d->mime = mimeFor(d->sps);
    if (!d->startTrackLocked()) {
      d->waitKey = true;
      return;
    }
  }
  if (key) ++d->keyframes;
  // Timeline = arrival time on this device. The player's clock runs in real time, also while a
  // still iPhone screen sends nothing; a timeline that skipped those gaps (100 ms cap) fell
  // behind it and every later frame was dropped as late ("Late frame ... A lot of buffers are
  // being dropped"), worse after each pause. Arrival time can't drift from that clock; the
  // start-up cushion keeps frames ahead of it.
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  if (d->wallStartUs < 0) d->wallStartUs = nowUs;
  const int64_t arrivalMs = (nowUs - d->wallStartUs) / 1000;
  // Latency control: a stall ("waiting") stops the player's clock and adds its length to the
  // latency for good. Measure it (arrival minus play position, every 10 frames) and, while it is
  // above target, give each frame a 2 ms earlier time: wins back ~60 ms/s, no frames dropped.
  // Only while frames flow (10 frames within 0.5 s): on a still screen the play position stays
  // on the last frame and the difference would grow without being latency.
  // (the position itself is read after queueing, without the lock: see below)
  bool sampleNow = false;
  if (d->started && d->queued % 10 == 0) {
    sampleNow = d->sampleArrivalMs >= 0 && arrivalMs - d->sampleArrivalMs < 500;
    d->sampleArrivalMs = arrivalMs;
    if (!sampleNow) d->latencyMs = -1;
  }
  if (d->started && d->latencyMs > d->cushionMs + 60) {
    d->pullbackMs += 2;
    ++d->pullbacks;
  }
  int64_t ms = arrivalMs - d->pullbackMs;
  if (ms <= d->lastPtsMs) ms = d->lastPtsMs + 1;  // must increase
  try {
    auto s = d->media->makeMediaSample(apmf::detail::ArrayView<uint8_t const>(sample->data(), sample->size()));
    s->setPresentationTimestamp(ms);
    s->setDecodingTimestamp(ms);
    s->setDuration(33);
    if (key) s->setSyncFrame();
    d->keepAlive.push_back(sample);
    while (d->keepAlive.size() > 2400) d->keepAlive.pop_front();  // video + audio, ~20 s
    auto track = d->track;
    auto element = d->element;
    const bool playNow = !d->started && d->cushionFrom >= 0 && ms - d->cushionFrom >= d->cushionMs;
    if (!d->started && d->cushionFrom < 0) d->cushionFrom = ms;
    // No player call with the lock held: a stuck player (e.g. while Safari's URL playback held the
    // decoder) then blocked statusJson, the HTTP server and mDNS with it, and the TV vanished from
    // the iPhone's list. Frames arrive on one thread, so order is kept.
    g.unlock();
    auto t0 = std::chrono::steady_clock::now();
    track->queueMediaSample(s);
    if (playNow) element->play();
    double posSec = sampleNow ? element->getCurrentTimeSecs() : -1;
    g.lock();
    if (posSec >= 0) {
      d->lastPosSec = posSec;
      d->latencyMs = arrivalMs - d->pullbackMs - static_cast<int64_t>(posSec * 1000);
    }
    if (playNow) {
      d->started = true;
      d->note("play after " + std::to_string(d->cushionMs) + " ms cushion");
    }
    double qms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (qms > d->queueMsMax) d->queueMsMax = qms;
    d->lastPtsMs = ms;
    ++d->queued;
    // No sound from the sender lately: keep the audio track ~100 ms ahead of the picture with
    // silence, or the player stops and waits for audio.
    if (d->audioTrack && ms - d->lastRealAudioMs > 300) {
      if (d->audioNextMs < 0 || d->audioNextMs < ms - 200.0) d->audioNextMs = static_cast<double>(ms);
      d->fillSilenceLocked(g, ms + 100.0);
    }
  } catch (std::exception const& e) {
    if (!g.owns_lock()) g.lock();
    ++d->queueErrors;
    d->fail("queue", e.what());
  } catch (...) {
    if (!g.owns_lock()) g.lock();
    ++d->queueErrors;
    d->fail("queue", "unknown exception");
  }
}

std::string MssePlayer::statusJson() const {
  auto d = impl_;
  // Never wait here: status feeds the HTTP server (and with it AirPlay discovery). If a media
  // call holds the lock, report the last snapshot.
  std::unique_lock<std::recursive_mutex> g(d->mu, std::try_to_lock);
  if (!g.owns_lock()) {
    std::lock_guard<std::mutex> c(d->cacheMu);
    return d->lastStatus.empty() ? std::string("{\"state\":\"busy\"}") : d->lastStatus;
  }
  // Cached values only: never call into the player here (see push).
  const double t = d->lastPosSec;
  const int64_t decoded = -1, dropped = -1, ready = -1, paused = -1;
  std::ostringstream o;
  o << "{\"state\":\"" << jsonEscape(d->state) << "\",\"initialized\":" << (d->initialized ? "true" : "false")
    << ",\"queued\":" << d->queued << ",\"skipped\":" << d->skipped << ",\"keyframes\":" << d->keyframes
    << ",\"audioQueued\":" << d->audioQueued << ",\"audioErrors\":" << d->audioErrors
    << ",\"audioReanchors\":" << d->audioReanchors << ",\"silenceFrames\":" << d->silenceFrames << ",\"audioTrack\":" << (d->audioTrack ? "true" : "false")
    << ",\"latencyMs\":" << d->latencyMs << ",\"cushionMs\":" << d->cushionMs
    << ",\"pullbackMs\":" << d->pullbackMs << ",\"queueErrors\":" << d->queueErrors << ",\"queueMsMax\":" << d->queueMsMax
    << ",\"lastPtsSec\":" << d->lastPtsMs / 1000.0 << ",\"t\":" << t << ",\"decoded\":" << decoded
    << ",\"dropped\":" << dropped << ",\"readyState\":" << ready << ",\"paused\":" << paused
    << ",\"error\":\"" << jsonEscape(d->lastError) << "\",\"events\":[";
  bool first = true;
  for (auto& e : d->events) {
    o << (first ? "" : ",") << "\"" << jsonEscape(e) << "\"";
    first = false;
  }
  o << "]}";
  std::lock_guard<std::mutex> c(d->cacheMu);
  d->lastStatus = o.str();
  return d->lastStatus;
}

}  // namespace ProbeNativeTurboModule
