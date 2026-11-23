#include "audio_player.h"

#if SCREENATIC_WITH_ALAC
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#else
class ALACDecoder {};  // complete type for std::unique_ptr when ALAC is not built
#endif

#include <android/log.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#define LOG_TAG "ScreenaticAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace screenatic {

namespace {
// AudioSpecificConfig for AAC-ELD 44.1 kHz stereo, 480 spf (same as UxPlay's GStreamer caps).
const uint8_t kAacEldConfig[] = {0xf8, 0xe8, 0x50, 0x00};
// AAC-LC 44.1 kHz stereo.
const uint8_t kAacLcConfig[] = {0x12, 0x10};
// ALAC magic cookie ('alac' atom + ALACSpecificConfig): 352 spf, 16-bit, pb 40, mb 10, kb 14,
// 2 ch, maxRun 255, 44100 Hz. Same as UxPlay's GStreamer caps; AirPlay never sends one.
const uint8_t kAlacCookie[] = {
    0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
constexpr uint32_t kAlacFrames = 352;

constexpr size_t kRingFrames = 44100;  // 1 s
constexpr size_t msToFrames(int ms) { return static_cast<size_t>(ms) * 44100 / 1000; }

int64_t nowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1'000;
}
}  // namespace

AudioPlayer::AudioPlayer() = default;
AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start(int format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((codec_ || alac_) && format == format_) return true;
    closeDecoder();
    if (!(format == kAlac ? openAlac() : openDecoder(format))) return false;
    if (!stream_ && !openStream()) {
        closeDecoder();
        return false;
    }
    format_ = format;
    return true;
}

void AudioPlayer::stop() {
    std::shared_ptr<oboe::AudioStream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closeDecoder();
        format_ = 0;
        stream = std::move(stream_);
    }
    if (stream) {  // outside the lock: close() waits for the callback to finish
        stream->requestStop();
        stream->close();
    }
    readPos_ = writePos_.load();
    priming_ = true;
}

void AudioPlayer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (codec_) AMediaCodec_flush(codec_);
    readPos_ = writePos_.load();  // consumer-side position; safe enough for a flush
    priming_ = true;
}

void AudioPlayer::setVolumeDb(float db) {
    // AirPlay sends -30..0 dB, -144 for mute. Map the slider like UxPlay/shairport-sync's
    // "tapered" volume: halving the slider halves perceived loudness (-10 dB), never louder
    // than a flat -30..0 dB mapping.
    float g;
    if (db <= -144.0f || db <= -30.0f) {
        g = 0.0f;
    } else if (db >= 0.0f) {
        g = 1.0f;
    } else {
        float frac = (30.0f + db) / 30.0f;
        float flat = -30.0f * (1.0f - frac);
        float tapered = 10.0f * std::log2(frac);
        g = std::pow(10.0f, std::max(flat, tapered) / 20.0f);
    }
    gain_ = g;
    LOGI("volume %.1f dB -> gain %.3f", db, g);
}

void AudioPlayer::submit(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = nowUs();
    if (lastSubmitUs_ != 0) maxGapUs_ = std::max(maxGapUs_, now - lastSubmitUs_);
    lastSubmitUs_ = now;
    ++frames_;
    if (now - lastStatsUs_ >= 10'000'000) {
        if (lastStatsUs_ != 0) logStatsLocked();
        lastStatsUs_ = now;
    }
    if (alac_) {
        decodeAlac(data, size);
        return;
    }
    if (!codec_) return;

    ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 5'000 /* us */);
    if (index < 0) {
        drainDecoder();
        index = AMediaCodec_dequeueInputBuffer(codec_, 5'000);
    }
    if (index >= 0) {
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, index, &cap);
        size_t n = std::min(size, cap);
        memcpy(buf, data, n);
        AMediaCodec_queueInputBuffer(codec_, index, 0, n, 0, 0);
    } else {
        ++busyDrops_;
    }
    drainDecoder();
}

void AudioPlayer::logStatsLocked() {
    // One line per 10 s: packets in, longest gap between packets (network/resend stalls), ring
    // underruns (audible gaps), trims (audio skipped to catch up) and decoder input drops.
    size_t level = writePos_.load() - readPos_.load();
    LOGI("stats 10s: ct=%d pkts=%u maxGap=%lldms level=%zums underruns=%u trims=%u (%llums) busyDrops=%u",
         format_, frames_, static_cast<long long>(maxGapUs_ / 1000), level * 1000 / kSampleRate,
         underruns_.exchange(0), trims_.exchange(0),
         static_cast<unsigned long long>(trimmedFrames_.exchange(0) * 1000 / kSampleRate), busyDrops_);
    frames_ = 0;
    maxGapUs_ = 0;
    busyDrops_ = 0;
}

bool AudioPlayer::openDecoder(int format) {
    const uint8_t* csd = nullptr;
    size_t csdLen = 0;
    switch (format) {
        case kAacEld: csd = kAacEldConfig; csdLen = sizeof(kAacEldConfig); break;
        case kAacLc:  csd = kAacLcConfig;  csdLen = sizeof(kAacLcConfig);  break;
        default:
            LOGW("audio format ct=%d not supported", format);
            return false;
    }
    codec_ = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!codec_) {
        LOGW("no AAC decoder");
        return false;
    }
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, kSampleRate);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, kChannels);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    AMediaFormat_setBuffer(f, "csd-0", csd, csdLen);
    media_status_t st = AMediaCodec_configure(codec_, f, nullptr, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGW("audio decoder configure/start failed (%d)", st);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    decoderChannels_ = kChannels;
    LOGI("audio decoder started: ct=%d", format);
    return true;
}

bool AudioPlayer::openStream() {
    ring_.assign(kRingFrames * kChannels, 0);
    readPos_ = 0;
    writePos_ = 0;
    priming_ = true;

    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(kChannels)
        ->setSampleRate(kSampleRate)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setUsage(oboe::Usage::Media)
        ->setContentType(oboe::ContentType::Movie)
        ->setDataCallback(this)
        ->setErrorCallback(this);
    oboe::Result r = b.openStream(stream_);
    if (r != oboe::Result::OK) {
        LOGW("openStream failed: %s", oboe::convertToText(r));
        stream_.reset();
        return false;
    }
    r = stream_->requestStart();
    if (r != oboe::Result::OK) {
        LOGW("requestStart failed: %s", oboe::convertToText(r));
        stream_->close();
        stream_.reset();
        return false;
    }
    LOGI("audio out: %s, %d Hz, burst %d frames", oboe::convertToText(stream_->getAudioApi()),
         stream_->getSampleRate(), stream_->getFramesPerBurst());
    return true;
}

void AudioPlayer::closeDecoder() {
    alac_.reset();
    if (!codec_) return;
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
}

#if SCREENATIC_WITH_ALAC
bool AudioPlayer::openAlac() {
    alac_ = std::make_unique<ALACDecoder>();
    int32_t err = alac_->Init(const_cast<uint8_t*>(kAlacCookie), sizeof(kAlacCookie));
    if (err != 0) {
        LOGW("ALAC init failed (%d)", err);
        alac_.reset();
        return false;
    }
    alacOut_.assign(4096 * kChannels, 0);  // max ALAC packet is far above AirPlay's 352
    LOGI("audio decoder started: ct=2 (ALAC)");
    return true;
}

void AudioPlayer::decodeAlac(const uint8_t* data, size_t size) {
    BitBuffer bits;
    BitBufferInit(&bits, const_cast<uint8_t*>(data), static_cast<uint32_t>(size));
    uint32_t outFrames = 0;
    int32_t err = alac_->Decode(&bits, reinterpret_cast<uint8_t*>(alacOut_.data()), kAlacFrames,
                                kChannels, &outFrames);
    if (err != 0) {
        LOGW("ALAC decode error %d", err);
        return;
    }
    writePcm(alacOut_.data(), outFrames, kChannels);
}
#else
bool AudioPlayer::openAlac() {
    LOGW("ALAC decoder not built in");
    return false;
}
void AudioPlayer::decodeAlac(const uint8_t*, size_t) {}
#endif

void AudioPlayer::drainDecoder() {
    AMediaCodecBufferInfo info;
    for (int guard = 0; guard < 16; ++guard) {
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (out >= 0) {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, out, &cap);
            if (buf && info.size > 0) {
                const auto* pcm = reinterpret_cast<const int16_t*>(buf + info.offset);
                size_t samples = static_cast<size_t>(info.size) / sizeof(int16_t);
                writePcm(pcm, samples / decoderChannels_, decoderChannels_);
            }
            AMediaCodec_releaseOutputBuffer(codec_, out, false);
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            int32_t ch = kChannels, rate = kSampleRate;
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
            AMediaFormat_delete(f);
            decoderChannels_ = ch > 0 ? ch : kChannels;
            LOGI("audio decoder output: %d ch, %d Hz", ch, rate);
        } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            break;
        } else if (out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            LOGW("audio dequeueOutputBuffer error %zd", out);
            break;
        }
    }
}

void AudioPlayer::writePcm(const int16_t* s, size_t frames, int channels) {
    if (ring_.empty()) return;
    // A burst (late packets released after a resend stall) can exceed the ring. Keep the newest
    // audio and overwrite the oldest: the consumer sees more than kMaxBufferedMs buffered and jumps
    // to the newest data anyway, so stale audio is never played.
    size_t w = writePos_.load(std::memory_order_relaxed);
    if (frames > kRingFrames) {
        s += (frames - kRingFrames) * channels;
        frames = kRingFrames;
    }
    for (size_t i = 0; i < frames; ++i) {
        size_t slot = ((w + i) % kRingFrames) * kChannels;
        int16_t left = s[i * channels];
        int16_t right = channels > 1 ? s[i * channels + 1] : left;
        ring_[slot] = left;
        ring_[slot + 1] = right;
    }
    writePos_.store(w + frames, std::memory_order_release);
}

oboe::DataCallbackResult AudioPlayer::onAudioReady(oboe::AudioStream*, void* audio, int32_t frames) {
    auto* out = static_cast<int16_t*>(audio);
    size_t r = readPos_.load(std::memory_order_relaxed);
    size_t w = writePos_.load(std::memory_order_acquire);
    size_t avail = w - r;

    if (priming_) {
        if (avail < msToFrames(kTargetMs)) {
            memset(out, 0, static_cast<size_t>(frames) * kChannels * sizeof(int16_t));
            return oboe::DataCallbackResult::Continue;
        }
        priming_ = false;
    }
    if (avail > msToFrames(kMaxBufferedMs)) {  // fell behind: jump to the target latency
        trims_.fetch_add(1, std::memory_order_relaxed);
        trimmedFrames_.fetch_add(avail - msToFrames(kTargetMs), std::memory_order_relaxed);
        r = w - msToFrames(kTargetMs);
        avail = w - r;
    }

    const float g = gain_.load(std::memory_order_relaxed);
    size_t n = std::min(static_cast<size_t>(frames), avail);
    for (size_t i = 0; i < n; ++i) {
        size_t slot = ((r + i) % kRingFrames) * kChannels;
        out[i * 2] = static_cast<int16_t>(ring_[slot] * g);
        out[i * 2 + 1] = static_cast<int16_t>(ring_[slot + 1] * g);
    }
    if (n < static_cast<size_t>(frames)) {  // underrun: silence and re-prime
        memset(out + n * kChannels, 0, (frames - n) * kChannels * sizeof(int16_t));
        priming_ = true;
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    readPos_.store(r + n, std::memory_order_release);
    return oboe::DataCallbackResult::Continue;
}

void AudioPlayer::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    // Output device changed (e.g. HDMI renegotiation). Reopen on the next frame.
    LOGW("audio stream closed: %s", oboe::convertToText(error));
    std::lock_guard<std::mutex> lock(mutex_);
    stream_.reset();
    if (codec_ || alac_) openStream();
}

}  // namespace screenatic
