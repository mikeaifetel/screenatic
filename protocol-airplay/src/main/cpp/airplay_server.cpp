#include "airplay_server.h"

#include <cstring>
#include <utility>
#include <vector>

#if SCREENHOP_WITH_UXPLAY
extern "C" {
#include "raop.h"
#include "dnssd.h"
#include "logger.h"
}
#endif

namespace screenhop {

AirPlayServer::AirPlayServer(AirPlayOptions options, AirPlayCallbacks callbacks)
    : opt_(std::move(options)), cb_(std::move(callbacks)) {}

AirPlayServer::~AirPlayServer() { stop(); }

// ------------------------------------------------------------------------------------------
// Event handling shared by both builds
// ------------------------------------------------------------------------------------------

void AirPlayServer::handleClientRequest(const char* deviceId, const char* model, const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    clientName_ = name ? name : (model ? model : "iPhone");
    log("client request: " + clientName_ + " (" + (model ? model : "?") + ", " + (deviceId ? deviceId : "?") + ")");
}

void AirPlayServer::handleCodec(bool h265) {
    log(std::string("mirror codec: ") + (h265 ? "H.265" : "H.264"));
    if (cb_.onVideoReset) cb_.onVideoReset();
    if (!mirroring_.exchange(true)) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            name = clientName_.empty() ? "iPhone" : clientName_;
        }
        if (cb_.onConnected) cb_.onConnected(name, ClientKind::Mirror);
    }
}

void AirPlayServer::handleVideo(const uint8_t* data, size_t size, uint64_t ntpRemoteNs, bool h265) {
    if (!mirroring_) handleCodec(h265);  // codec callback can be skipped on reconnects
    if (cb_.onVideo) cb_.onVideo(data, size, static_cast<int64_t>(ntpRemoteNs / 1000), h265);
}

void AirPlayServer::handleVideoSize(float width, float height) {
    log("sender reports size " + std::to_string(static_cast<int>(width)) + "x" + std::to_string(static_cast<int>(height)));
    if (cb_.onVideoSize && width > 0 && height > 0) cb_.onVideoSize(static_cast<int>(width), static_cast<int>(height));
}

void AirPlayServer::handleMirrorStopped() {
    if (cb_.onVideoReset) cb_.onVideoReset();
    if (mirroring_.exchange(false) && cb_.onDisconnected) cb_.onDisconnected();
}

void AirPlayServer::handleConnectionOpened() { ++openConnections_; }

void AirPlayServer::handleConnectionClosed() {
    if (--openConnections_ <= 0) {
        openConnections_ = 0;
        handleMirrorStopped();
        audioCt_ = 0;
        if (cb_.onAudioStop) cb_.onAudioStop();
    }
}

void AirPlayServer::handleAudioFormat(int ct) {
    log("audio format ct=" + std::to_string(ct));
    audioCt_ = ct;
    if (cb_.onAudioFormat) cb_.onAudioFormat(ct);
}

void AirPlayServer::handleAudio(const uint8_t* data, size_t size) {
    // Same sanity check as UxPlay's renderer: first byte identifies the codec.
    bool valid;
    switch (audioCt_.load()) {
        case 8: {  // AAC-ELD: 0x8c-0x8e on current iOS, 0x80-0x82 on iOS 9/10
            const uint8_t b0 = data[0];
            valid = (b0 >= 0x8c && b0 <= 0x8e) || (b0 >= 0x80 && b0 <= 0x82);
            break;
        }
        case 2: valid = data[0] == 0x20; break;                                       // ALAC
        case 4: valid = data[0] == 0xFF; break;                                       // AAC-LC
        default: valid = true; break;
    }
    if (!valid) {
        if (++badAudioFrames_ % 100 == 1) log("invalid audio frame skipped, first byte " + std::to_string(data[0]));
        return;
    }
    if (cb_.onAudio) cb_.onAudio(data, size);
}

void AirPlayServer::handlePlayUrl(const char* location, float startSeconds) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        name = clientName_.empty() ? "iPhone" : clientName_;
    }
    if (cb_.onPlayUrl && location) cb_.onPlayUrl(name, location, static_cast<int64_t>(startSeconds * 1000));
}

#if SCREENHOP_WITH_UXPLAY

// ------------------------------------------------------------------------------------------
// UxPlay callback trampolines. UxPlay calls several of these without a null check
// (video_set_codec, conn_feedback, audio_set_client_volume, on_video_acquire_playback_info,
// on_video_playlist_remove), so every slot gets a function.
// ------------------------------------------------------------------------------------------

namespace {

AirPlayServer* self(void* cls) { return static_cast<AirPlayServer*>(cls); }

void cbConnInit(void* cls) { self(cls)->handleConnectionOpened(); }
void cbConnDestroy(void* cls) { self(cls)->handleConnectionClosed(); }
void cbConnFeedback(void*) {}
void cbConnReset(void* cls, int reason) {
    self(cls)->log("connection reset, reason " + std::to_string(reason));
    self(cls)->handleMirrorStopped();
}
void cbReportClient(void* cls, char* deviceid, char* model, char* name, bool* admit) {
    self(cls)->handleClientRequest(deviceid, model, name);
    *admit = true;  // TODO: block list / "allow this device?" prompt
}

int cbVideoSetCodec(void* cls, video_codec_t codec) {
    self(cls)->handleCodec(codec == VIDEO_CODEC_H265);
    return 0;
}
void cbVideoProcess(void* cls, raop_ntp_t*, video_decode_struct* d) {
    if (!d || !d->data || d->data_len <= 0) return;
    self(cls)->handleVideo(d->data, static_cast<size_t>(d->data_len), d->ntp_time_remote, d->is_h265);
}
void cbVideoReportSize(void* cls, float*, float*, float* w, float* h) {
    if (w && h) self(cls)->handleVideoSize(*w, *h);
}
void cbVideoReset(void* cls, reset_type_t type) {
    self(cls)->log("video reset, type " + std::to_string(static_cast<int>(type)));
    if (type != RESET_TYPE_ON_VIDEO_PLAY) self(cls)->handleMirrorStopped();
}
void cbVideoFlush(void*) {}
void cbVideoPause(void*) {}
void cbVideoResume(void*) {}

// Audio: AAC-ELD (mirroring) decoded by AudioPlayer; ALAC (audio-only AirPlay) still TODO.
void cbAudioProcess(void* cls, raop_ntp_t*, audio_decode_struct* d) {
    if (d && d->data && d->data_len > 0) self(cls)->handleAudio(d->data, static_cast<size_t>(d->data_len));
}
void cbAudioFlush(void* cls) { self(cls)->handleAudioFlush(); }
double cbAudioClientVolume(void*) { return 0.0; }  // initial volume in dB, 0 = full
void cbAudioSetVolume(void* cls, float db) { self(cls)->handleAudioVolume(db); }
void cbAudioGetFormat(void* cls, unsigned char* ct, unsigned short* spf, bool* usingScreen, bool* isMedia, uint64_t*) {
    self(cls)->log("audio: ct=" + std::to_string(ct ? *ct : -1) + " spf=" + std::to_string(spf ? *spf : 0) +
                   " screen=" + std::to_string(usingScreen ? *usingScreen : 0) + " media=" + std::to_string(isMedia ? *isMedia : 0));
    if (ct) self(cls)->handleAudioFormat(*ct);
}
void cbAudioMetadata(void*, const void*, int) {}
void cbAudioCoverart(void*, const void*, int) {}
void cbAudioStopCoverart(void*) {}
void cbAudioProgress(void*, uint32_t*, uint32_t*, uint32_t*) {}

// HLS / URL casting: plumbing in place, advertised from M1.
void cbOnVideoPlay(void* cls, const char* location, const float start) { self(cls)->handlePlayUrl(location, start); }
void cbOnVideoScrub(void*, const float) {}
void cbOnVideoRate(void*, const float) {}
void cbOnVideoStop(void* cls) { self(cls)->handleMirrorStopped(); }
void cbOnPlaybackInfo(void*, playback_info_t* info) {
    if (!info) return;
    info->duration = 0;
    info->position = 0;
    info->rate = 0;
    info->ready_to_play = false;
    info->playback_buffer_empty = true;
    info->playback_buffer_full = false;
    info->playback_likely_to_keep_up = false;
    info->num_loaded_time_ranges = 0;
    info->num_seekable_time_ranges = 0;
}
float cbOnPlaylistRemove(void*) { return 0.0f; }

void cbLog(void* cls, int level, const char* msg) {
    if (level <= LOGGER_INFO && msg) self(cls)->log(msg);
}

bool parseMac(const std::string& s, std::vector<char>& out) {
    out.clear();
    unsigned int b[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (unsigned int v : b) out.push_back(static_cast<char>(v));
    return true;
}

/** DNS-SD TXT wire format ([len][key=value]...) -> map. */
std::map<std::string, std::string> parseTxt(const char* data, int len) {
    std::map<std::string, std::string> kv;
    int i = 0;
    while (data && i < len) {
        int n = static_cast<unsigned char>(data[i]);
        if (i + 1 + n > len) break;
        std::string entry(data + i + 1, n);
        auto eq = entry.find('=');
        if (eq == std::string::npos) kv[entry] = "";
        else kv[entry.substr(0, eq)] = entry.substr(eq + 1);
        i += 1 + n;
    }
    return kv;
}

}  // namespace

bool AirPlayServer::start(int& port) {
    if (raop_) return false;

    std::vector<char> hw;
    if (!parseMac(opt_.deviceId, hw)) {
        log("bad device id " + opt_.deviceId);
        return false;
    }

    // dnssd: builds the TXT records (publishing itself is done by NsdManager in Kotlin).
    int err = 0;
    auto* d = dnssd_init(opt_.deviceName.c_str(), static_cast<int>(opt_.deviceName.size()),
                         hw.data(), static_cast<int>(hw.size()), &err, /*pin_pw*/ 0);
    if (!d || err) {
        log("dnssd_init failed: " + std::to_string(err));
        return false;
    }
    // Same feature tuning as uxplay.cpp's defaults: no HLS yet, optional H.265, legacy pairing on.
    dnssd_set_airplay_features(d, 0, 0);
    dnssd_set_airplay_features(d, 4, 0);
    dnssd_set_airplay_features(d, 42, opt_.h265 ? 1 : 0);
    dnssd_set_airplay_features(d, 27, 1);

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cls = this;
    cbs.conn_init = cbConnInit;
    cbs.conn_destroy = cbConnDestroy;
    cbs.conn_feedback = cbConnFeedback;
    cbs.conn_reset = cbConnReset;
    cbs.report_client_request = cbReportClient;
    cbs.video_set_codec = cbVideoSetCodec;
    cbs.video_process = cbVideoProcess;
    cbs.video_report_size = cbVideoReportSize;
    cbs.video_reset = cbVideoReset;
    cbs.video_flush = cbVideoFlush;
    cbs.video_pause = cbVideoPause;
    cbs.video_resume = cbVideoResume;
    cbs.audio_process = cbAudioProcess;
    cbs.audio_flush = cbAudioFlush;
    cbs.audio_set_client_volume = cbAudioClientVolume;
    cbs.audio_set_volume = cbAudioSetVolume;
    cbs.audio_get_format = cbAudioGetFormat;
    cbs.audio_set_metadata = cbAudioMetadata;
    cbs.audio_set_coverart = cbAudioCoverart;
    cbs.audio_stop_coverart_rendering = cbAudioStopCoverart;
    cbs.audio_set_progress = cbAudioProgress;
    cbs.on_video_play = cbOnVideoPlay;
    cbs.on_video_scrub = cbOnVideoScrub;
    cbs.on_video_rate = cbOnVideoRate;
    cbs.on_video_stop = cbOnVideoStop;
    cbs.on_video_acquire_playback_info = cbOnPlaybackInfo;
    cbs.on_video_playlist_remove = cbOnPlaylistRemove;

    raop_t* r = raop_init(&cbs);
    if (!r) {
        log("raop_init failed");
        dnssd_destroy(d);
        return false;
    }
    raop_set_log_callback(r, cbLog, this);
    raop_set_log_level(r, LOGGER_INFO);
    if (raop_init2(r, /*nohold: newest sender wins*/ 1, opt_.deviceId.c_str(), opt_.keyFile.c_str())) {
        log("raop_init2 failed");
        raop_destroy(r);
        dnssd_destroy(d);
        return false;
    }
    // Defaults sent to the sender: 1920x1080, 60 Hz, 30 fps max.
    raop_set_plist(r, "width", 1920);
    raop_set_plist(r, "height", 1080);

    unsigned short tcp[2] = {0, 0};      // 0 = pick free ports
    unsigned short udp[3] = {0, 0, 0};
    raop_set_tcp_ports(r, tcp);
    raop_set_udp_ports(r, udp);
    unsigned short p = raop_get_port(r);
    if (raop_start_httpd(r, &p) < 0 || p == 0) {
        log("raop_start_httpd failed");
        raop_destroy(r);
        dnssd_destroy(d);
        return false;
    }
    raop_set_port(r, p);
    raop_set_dnssd(r, d);  // copies the public key into the TXT records

    // With the shim these only build TXT records; NsdManager publishes them.
    dnssd_register_raop(d, p);
    dnssd_register_airplay(d, p);

    raop_ = r;
    dnssd_ = d;
    port = p;
    log("AirPlay server listening on port " + std::to_string(p));
    return true;
}

void AirPlayServer::stop() {
    if (raop_) {
        raop_destroy(static_cast<raop_t*>(raop_));
        raop_ = nullptr;
    }
    if (dnssd_) {
        auto* d = static_cast<dnssd_t*>(dnssd_);
        dnssd_unregister_raop(d);
        dnssd_unregister_airplay(d);
        dnssd_destroy(d);
        dnssd_ = nullptr;
    }
    mirroring_ = false;
    openConnections_ = 0;
}

void AirPlayServer::disconnectClients() {
    if (!raop_) return;
    raop_remove_known_connections(static_cast<raop_t*>(raop_));
    handleMirrorStopped();
}

std::map<std::string, std::string> AirPlayServer::airplayTxt() const {
    if (!dnssd_) return {};
    int len = 0;
    const char* txt = dnssd_get_airplay_txt(static_cast<dnssd_t*>(dnssd_), &len);
    return parseTxt(txt, len);
}

std::map<std::string, std::string> AirPlayServer::raopTxt() const {
    if (!dnssd_) return {};
    int len = 0;
    const char* txt = dnssd_get_raop_txt(static_cast<dnssd_t*>(dnssd_), &len);
    return parseTxt(txt, len);
}

#else  // ---------------- stub (no UxPlay / deps) ----------------

bool AirPlayServer::start(int& port) {
    log("STUB AirPlay server: advertising only, no protocol handling");
    port = 7000;
    return true;
}

void AirPlayServer::stop() {}
void AirPlayServer::disconnectClients() {}

std::map<std::string, std::string> AirPlayServer::airplayTxt() const {
    return {{"deviceid", opt_.deviceId}, {"features", "0x5A7FFEE6"}, {"model", "AppleTV3,2"},
            {"srcvers", "220.68"}, {"flags", "0x4"}, {"vv", "2"}};
}

std::map<std::string, std::string> AirPlayServer::raopTxt() const {
    return {{"ch", "2"}, {"cn", "0,1,2,3"}, {"et", "0,3,5"}, {"ft", "0x5A7FFEE6"},
            {"am", "AppleTV3,2"}, {"sf", "0x4"}, {"tp", "UDP"}, {"vs", "220.68"}, {"vv", "2"}};
}

#endif

}  // namespace screenhop
