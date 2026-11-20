#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace screenhop {

enum class ClientKind : int { Mirror = 0, Audio = 1 };

/** Events raised by the protocol core (on its own threads). */
struct AirPlayCallbacks {
    std::function<void(const std::string& clientName, ClientKind)> onConnected;
    std::function<void(const std::string& clientName, const std::string& url, int64_t startMs)> onPlayUrl;
    std::function<void()> onDisconnected;
    /** One access unit, Annex-B with SPS/PPS(/VPS) inline. */
    std::function<void(const uint8_t* data, size_t size, int64_t ptsUs, bool h265)> onVideo;
    /** Mirrored picture size, for aspect-ratio-correct display. */
    std::function<void(int width, int height)> onVideoSize;
    /** Drop decoder state (stream restarted or stopped). */
    std::function<void()> onVideoReset;
    /** Audio stream format announced (UxPlay ct: 8 = AAC-ELD, 4 = AAC-LC, 2 = ALAC). */
    std::function<void(int ct)> onAudioFormat;
    /** One compressed audio frame. */
    std::function<void(const uint8_t* data, size_t size)> onAudio;
    /** AirPlay volume in dB (-30..0, -144 = mute). */
    std::function<void(float db)> onAudioVolume;
    std::function<void()> onAudioFlush;
    /** All connections gone: release the audio output. */
    std::function<void()> onAudioStop;
    std::function<void(const std::string&)> onLog;
};

struct AirPlayOptions {
    std::string deviceName;
    /** MAC-style id "AA:BB:CC:DD:EE:FF" (also the AirPlay deviceid). */
    std::string deviceId;
    /** PEM file holding the persistent ed25519 identity key (created on first run). */
    std::string keyFile;
    /** Advertise H.265 mirroring (only if the TV has a HEVC decoder). */
    bool h265 = false;
};

/**
 * Wrapper over UxPlay's raop server (lib/raop.h, v1.73.7).
 * Built as a stub when SCREENHOP_WITH_UXPLAY is off.
 */
class AirPlayServer {
public:
    AirPlayServer(AirPlayOptions options, AirPlayCallbacks callbacks);
    ~AirPlayServer();

    /** Starts listening. AirPlay and RAOP share one port. */
    bool start(int& port);
    void stop();
    void disconnectClients();

    /** TXT records for `_airplay._tcp` / `_raop._tcp` (valid after start()). */
    std::map<std::string, std::string> airplayTxt() const;
    std::map<std::string, std::string> raopTxt() const;

    // ---- called from UxPlay callback trampolines (airplay_server.cpp) ----
    void handleClientRequest(const char* deviceId, const char* model, const char* name);
    void handleCodec(bool h265);
    void handleVideo(const uint8_t* data, size_t size, uint64_t ntpRemoteNs, bool h265);
    void handleVideoSize(float width, float height);
    void handleMirrorStopped();
    void handleConnectionOpened();
    void handleConnectionClosed();
    void handlePlayUrl(const char* location, float startSeconds);
    void handleAudioFormat(int ct);
    void handleAudio(const uint8_t* data, size_t size);
    void handleAudioVolume(float db) { if (cb_.onAudioVolume) cb_.onAudioVolume(db); }
    void handleAudioFlush() { if (cb_.onAudioFlush) cb_.onAudioFlush(); }
    void log(const std::string& msg) { if (cb_.onLog) cb_.onLog(msg); }

private:
    AirPlayOptions opt_;
    AirPlayCallbacks cb_;
    void* raop_ = nullptr;   // raop_t*
    void* dnssd_ = nullptr;  // dnssd_t*

    std::mutex mutex_;
    std::string clientName_;
    std::atomic<int> openConnections_{0};
    std::atomic<bool> mirroring_{false};
    std::atomic<int> audioCt_{0};
    std::atomic<int> badAudioFrames_{0};
};

}  // namespace screenhop
