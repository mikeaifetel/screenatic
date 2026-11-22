#include "video_decoder.h"

#include <android/log.h>
#include <media/NdkMediaFormat.h>
#include <cstring>

#define LOG_TAG "ScreenaticDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace screenatic {

VideoDecoder::~VideoDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    releaseCodec();
    if (window_) ANativeWindow_release(window_);
}

void VideoDecoder::setWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mutex_);
    releaseCodec();  // codec is bound to its output surface; restart on change
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    if (!window_) return;
    ANativeWindow_acquire(window_);
    if (!gop_.empty() && ensureStarted(gopCodec_)) {
        LOGI("surface attached: replaying %zu cached access units (%zu bytes)", gop_.size(), gopBytes_);
        for (const auto& au : gop_) queue(au.data.data(), au.data.size(), au.ptsUs);
        haveKeyframe_ = true;
    }
}

void VideoDecoder::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    releaseCodec();
    gop_.clear();
    gopBytes_ = 0;
}

void VideoDecoder::cache(const uint8_t* data, size_t size, int64_t ptsUs, bool key, Codec codec) {
    static constexpr size_t kMaxBytes = 24u << 20;  // ~24 MB
    static constexpr size_t kMaxUnits = 900;        // ~30 s at 30 fps
    if (key) {
        gop_.clear();
        gopBytes_ = 0;
        gopCodec_ = codec;
    } else if (gop_.empty()) {
        return;  // no keyframe to anchor on
    }
    if (gopBytes_ + size > kMaxBytes || gop_.size() >= kMaxUnits) {
        // A partial GOP is not decodable, so drop it and wait for the next keyframe.
        // Only matters if the Surface is lost mid-stream.
        if (window_ == nullptr) LOGW("GOP cache full (%zu units, %zu bytes); waiting for next keyframe", gop_.size(), gopBytes_);
        gop_.clear();
        gopBytes_ = 0;
        return;
    }
    gop_.push_back({std::vector<uint8_t>(data, data + size), ptsUs});
    gopBytes_ += size;
}

void VideoDecoder::submit(const uint8_t* data, size_t size, int64_t ptsUs, Codec codec) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool key = hasParameterSets(data, size, codec);
    cache(data, size, ptsUs, key, codec);
    if (!window_ || !ensureStarted(codec)) return;
    if (key) haveKeyframe_ = true;
    if (!haveKeyframe_) return;  // P-frames before any SPS/PPS only produce garbage
    queue(data, size, ptsUs);
}

void VideoDecoder::queue(const uint8_t* data, size_t size, int64_t ptsUs) {
    // Wait rather than drop: a dropped reference frame freezes the picture until the next IDR.
    ssize_t index = -1;
    for (int attempt = 0; attempt < 25 && index < 0; ++attempt) {
        index = AMediaCodec_dequeueInputBuffer(codec_, 20'000 /* us */);
    }
    if (index < 0) {
        if (++dropped_ % 30 == 1) LOGW("no input buffer after 500 ms (status %zd), dropped %d frames", index, dropped_);
        return;
    }
    size_t capacity = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, index, &capacity);
    size_t n = size < capacity ? size : capacity;
    if (n < size) LOGW("access unit truncated: %zu > %zu", size, capacity);
    memcpy(buf, data, n);
    AMediaCodec_queueInputBuffer(codec_, index, 0, n, ptsUs, 0);
}

bool VideoDecoder::ensureStarted(Codec codec) {
    if (codec_ && codec == currentCodec_) return true;
    releaseCodec();

    const char* mime = codec == Codec::H265 ? "video/hevc" : "video/avc";
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) return false;

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    // Nominal size; the real size comes in-band with SPS. Max size enables adaptive
    // playback so portrait phone streams (e.g. 886x1920) don't need a reconfigure.
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(fmt, "max-width", 1920);
    AMediaFormat_setInt32(fmt, "max-height", 1920);
    // No vendor "low-latency" key for now: it selects a less-tested path in some
    // drivers. Revisit per device once mirroring is stable.

    media_status_t st = AMediaCodec_configure(codec_, fmt, window_, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGW("decoder configure/start failed (%d)", st);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    currentCodec_ = codec;
    haveKeyframe_ = false;
    dropped_ = 0;
    rendered_ = 0;
    outputRunning_ = true;
    outputThread_ = std::thread(&VideoDecoder::outputLoop, this, codec_);
    LOGI("decoder started: %s", mime);
    return true;
}

void VideoDecoder::outputLoop(AMediaCodec* codec) {
    AMediaCodecBufferInfo info;
    while (outputRunning_) {
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 10'000 /* us */);
        if (out >= 0) {
            // Render immediately: mirroring favours latency over smoothness.
            AMediaCodec_releaseOutputBuffer(codec, out, true);
            int n = ++rendered_;
            if (n == 1 || n % 600 == 0) LOGI("rendered %d frames", n);
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            reportFormat(codec);
        } else if (out != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                   out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            LOGW("dequeueOutputBuffer error %zd", out);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void VideoDecoder::reportFormat(AMediaCodec* codec) {
    AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
    if (!f) return;
    LOGI("output format: %s", AMediaFormat_toString(f));
    int32_t w = 0, h = 0, left = 0, top = 0, right = -1, bottom = -1;
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
    if (AMediaFormat_getInt32(f, "crop-left", &left) && AMediaFormat_getInt32(f, "crop-right", &right) &&
        AMediaFormat_getInt32(f, "crop-top", &top) && AMediaFormat_getInt32(f, "crop-bottom", &bottom)) {
        w = right - left + 1;
        h = bottom - top + 1;
    }
    AMediaFormat_delete(f);
    if (w > 0 && h > 0 && sizeListener_) sizeListener_(w, h);
}

void VideoDecoder::releaseCodec() {
    if (!codec_) return;
    outputRunning_ = false;
    if (outputThread_.joinable()) outputThread_.join();  // stop draining before stopping the codec
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    haveKeyframe_ = false;
}

bool VideoDecoder::hasParameterSets(const uint8_t* d, size_t size, Codec codec) {
    // Parameter sets lead the access unit (UxPlay prepends them); SEI may precede them on Macs.
    const size_t limit = size < 1024 ? size : 1024;
    for (size_t i = 0; i + 4 < limit; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const uint8_t h = d[i + 3];
            if (codec == Codec::H264 && (h & 0x1F) == 7) return true;          // SPS
            if (codec == Codec::H265 && ((h >> 1) & 0x3F) == 32) return true;  // VPS
            i += 2;
        }
    }
    return false;
}

}  // namespace screenatic
