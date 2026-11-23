#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace ProbeNativeTurboModule {

/**
 * Minimal fragmented-MP4 writer for live H.264 (AirPlay mirroring) into Vega's MSE.
 *
 * Input: one access unit at a time, Annex-B (start codes) with SPS/PPS inline before IDRs, as
 * UxPlay delivers it. Output: an init segment (ftyp + moov) whenever SPS/PPS change (first
 * frame, rotation, resolution change), then one moof + mdat per frame.
 *
 * Timing: frames arrive irregularly (an idle screen sends almost nothing), so each sample gets
 * the interval since the previous frame as its duration, clamped to 1..600 ms (see push()). The
 * player runs in sequence mode and the JS side speeds playback up when it falls behind.
 */
class Fmp4Muxer {
public:
  /** Appends the bytes to send to MSE for this frame (possibly an init segment first) to `out`.
   *  Frames before the first SPS/PPS are dropped. */
  void push(const uint8_t* data, size_t size, int64_t ptsUs, std::vector<uint8_t>& out);
  /** Picture size from the sender (used in tkhd/avc1; the decoder reads the SPS). */
  void setSize(int width, int height) { width_ = width; height_ = height; }
  /** Forget SPS/PPS and timing (new session). */
  void reset();

  uint64_t frames() const { return frames_; }
  uint64_t keyframes() const { return keyframes_; }
  /** Seconds between the last two keyframes (-1 until there are two). */
  double lastKeyIntervalSec() const { return lastKeyIntervalSec_; }
  /** Start times (seconds on the muxed timeline, which is the MSE sequence-mode timeline as long
   *  as every byte is appended) of the most recent keyframes, oldest first. Trimming the
   *  SourceBuffer is only safe up to one of these. */
  std::vector<double> keyTimes() const { return std::vector<double>(keyTimes_.begin(), keyTimes_.end()); }
  uint32_t initSegments() const { return inits_; }

private:
  void writeInit(std::vector<uint8_t>& out) const;

  std::vector<uint8_t> sps_, pps_;
  int width_ = 0, height_ = 0;
  int64_t lastPtsUs_ = -1;
  uint64_t decodeTime_ = 0;  // 90 kHz
  uint32_t sequence_ = 0;
  uint64_t frames_ = 0;
  uint32_t inits_ = 0;
  bool needInit_ = true;
  uint64_t keyframes_ = 0;
  int64_t lastKeyPtsUs_ = -1;
  double lastKeyIntervalSec_ = -1;
  std::deque<double> keyTimes_;
};

/**
 * Audio-only fragmented MP4 with FLAC (ISO/IEC 14496-12 + the "fLaC"/"dfLa" mapping), the one
 * audio format Vega's MSE actually plays. Mirrored AirPlay audio is decoded to PCM, encoded with
 * libFLAC (fixed 480-sample blocks, one per AAC-ELD packet) and each FLAC frame becomes a sample.
 * Played by a separate AudioPlayer so a still screen (no video frames) never stalls the sound.
 */
class FlacMp4Muxer {
public:
  /** STREAMINFO metadata block body (34 bytes), from libFLAC's first metadata write. */
  void setStreamInfo(const uint8_t* body, size_t size);
  /** One encoded FLAC frame of `samples` per channel. Appends output to `out` every
   *  kFramesPerFragment frames (an init segment first, after setStreamInfo). */
  void addFrame(const uint8_t* data, size_t size, uint32_t samples, std::vector<uint8_t>& out);
  void reset();

  uint64_t fragments() const { return fragments_; }

  static constexpr int kFramesPerFragment = 4;  // 4 x 480 samples ~ 44 ms

private:
  void writeInit(std::vector<uint8_t>& out) const;
  void flush(std::vector<uint8_t>& out);

  std::vector<uint8_t> streamInfo_;
  bool needInit_ = true;
  std::vector<uint8_t> pendingData_;
  std::vector<uint32_t> pendingSizes_, pendingSamples_;
  uint64_t decodeTime_ = 0;  // in samples (timescale 44100)
  uint32_t sequence_ = 0;
  uint64_t fragments_ = 0;
};

}  // namespace ProbeNativeTurboModule
