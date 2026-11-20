#pragma once

#include <media/NdkMediaCodec.h>
#include <oboe/Oboe.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class ALACDecoder;  // Apple ALAC (Apache-2.0), third_party/alac

namespace screenhop {

/**
 * AirPlay audio: compressed frames -> AMediaCodec -> PCM ring buffer -> Oboe output.
 *
 * Formats (UxPlay "ct"): 8 = AAC-ELD 44.1 kHz stereo, 480 samples/frame (screen mirroring),
 * 4 = AAC-LC, 2 = ALAC 44.1 kHz/16-bit stereo, 352 samples/frame (audio-only AirPlay, e.g. Music).
 * AAC goes through AMediaCodec; ALAC through Apple's open-source decoder (no platform ALAC
 * decoder on most TVs).
 *
 * Latency: frames are played as they arrive with a small jitter buffer. If the buffer grows
 * past kMaxBufferedMs (sender burst, decoder hiccup) it is trimmed back to kTargetMs so audio
 * never drifts behind the mirrored picture.
 */
class AudioPlayer : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    enum Format : int { kAlac = 2, kAacLc = 4, kAacEld = 8 };

    AudioPlayer();  // out of line: ALACDecoder is incomplete here (unique_ptr member)
    ~AudioPlayer() override;

    /** (Re)start for a format; returns false if this format can't be decoded here. */
    bool start(int format);
    void stop();
    /** One compressed frame from the sender. */
    void submit(const uint8_t* data, size_t size);
    /** Drop buffered audio (sender paused / seeked). */
    void flush();
    /** AirPlay volume in dB: -30..0, -144 = mute. */
    void setVolumeDb(float db);

    // Oboe callbacks (audio thread)
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audio, int32_t frames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    static constexpr int kSampleRate = 44100;
    static constexpr int kChannels = 2;
    static constexpr int kTargetMs = 80;
    static constexpr int kMaxBufferedMs = 250;

    bool openDecoder(int format);
    bool openAlac();
    void decodeAlac(const uint8_t* data, size_t size);
    bool openStream();
    void closeDecoder();
    void drainDecoder();
    void writePcm(const int16_t* samples, size_t frames, int channels);

    std::mutex mutex_;
    int format_ = 0;
    AMediaCodec* codec_ = nullptr;
    std::unique_ptr<ALACDecoder> alac_;
    std::vector<int16_t> alacOut_;
    int decoderChannels_ = kChannels;
    std::shared_ptr<oboe::AudioStream> stream_;

    // Single-producer (decoder) / single-consumer (Oboe callback) ring of interleaved S16 stereo.
    std::vector<int16_t> ring_;
    std::atomic<size_t> readPos_{0};   // in frames, monotonically increasing
    std::atomic<size_t> writePos_{0};
    std::atomic<bool> priming_{true};  // wait for kTargetMs before first playback / after underrun
    std::atomic<float> gain_{1.0f};
};

}  // namespace screenhop
