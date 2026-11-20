#pragma once

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace screenhop {

/**
 * Hardware H.264/H.265 decoder rendering straight to an ANativeWindow.
 * Fed with Annex-B access units from the AirPlay mirroring stream.
 *
 * - A dedicated output thread releases decoded frames continuously. Never leave output
 *   buffers queued: on Amlogic (Chromecast with Google TV) a starved display-buffer pool
 *   crashed the kernel H.264 driver (amvdec_mh264_v4l) and rebooted the device.
 * - Everything since the last keyframe (access unit with SPS/PPS, or VPS for H.265) is kept
 *   (a GOP cache), so decoding can start or restart when the Surface shows up after the stream
 *   began. Replaying only the keyframe is not enough: later P-frames reference the frames in
 *   between, and the Amlogic decoder then drops everything until the next IDR, which iOS
 *   rarely sends (the picture froze on the first frame).
 * - Input frames are never dropped on a busy decoder; it waits instead, for the same reason.
 */
class VideoDecoder {
public:
    enum class Codec { H264, H265 };

    ~VideoDecoder();

    /** Called with the displayed picture size (crop applied) whenever it changes. */
    void setSizeListener(std::function<void(int, int)> listener) { sizeListener_ = std::move(listener); }

    /** Attach (or detach with nullptr) the output window. Replays the cached keyframe. */
    void setWindow(ANativeWindow* window);

    /** Push one access unit (Annex-B). Without a window only keyframes are kept. */
    void submit(const uint8_t* data, size_t size, int64_t ptsUs, Codec codec = Codec::H264);

    /** Stream ended or restarted: drop codec state and cached keyframe. */
    void reset();

private:
    bool ensureStarted(Codec codec);
    void queue(const uint8_t* data, size_t size, int64_t ptsUs);
    void releaseCodec();
    void outputLoop(AMediaCodec* codec);
    void reportFormat(AMediaCodec* codec);
    static bool hasParameterSets(const uint8_t* data, size_t size, Codec codec);

    std::mutex mutex_;
    ANativeWindow* window_ = nullptr;
    AMediaCodec* codec_ = nullptr;
    Codec currentCodec_ = Codec::H264;
    bool haveKeyframe_ = false;  // decoder has seen parameter sets since (re)start
    int dropped_ = 0;

    std::thread outputThread_;
    std::atomic<bool> outputRunning_{false};
    std::atomic<int> rendered_{0};
    std::function<void(int, int)> sizeListener_;

    struct AccessUnit {
        std::vector<uint8_t> data;
        int64_t ptsUs;
    };
    std::deque<AccessUnit> gop_;
    size_t gopBytes_ = 0;
    Codec gopCodec_ = Codec::H264;
    void cache(const uint8_t* data, size_t size, int64_t ptsUs, bool key, Codec codec);
};

}  // namespace screenhop
