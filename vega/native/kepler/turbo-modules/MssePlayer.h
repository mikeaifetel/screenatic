#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ProbeNativeTurboModule {

/**
 * Plays the mirror's H.264 through Vega's W3C Media API in MSSE mode (Media Sample Source
 * Extension): every access unit is queued to the player as one sample, decoded and dropped.
 *
 * Why not MSE: Vega's SourceBuffer evicts at a fixed 100 MB and can only drop data up to a
 * keyframe, while an iPhone playing a video sends none for minutes, so the picture froze after
 * about a minute. MSSE keeps only a few seconds queued, so there is nothing to fill up.
 *
 * The picture goes to the KeplerVideoSurfaceView created in JS: its onSurfaceViewCreated handle
 * is a window handle, turned into an IMediaSurface here.
 */
class MssePlayer {
public:
  MssePlayer();
  ~MssePlayer();

  /** Creates the player and binds it to the JS surface (window handle as a decimal string).
   *  Returns a one-line result; the player initializes asynchronously (see statusJson). */
  std::string attach(const std::string& windowHandle);
  /** One H.264 access unit (Annex-B) from the sender, sender timestamp in microseconds. */
  void push(const uint8_t* data, size_t size, int64_t ptsUs);
  /** Internal: push without updating the keyframe cache (replaying it). */
  void pushFrame(const uint8_t* data, size_t size, bool fromCache);
  /** One FLAC frame of the mirror's audio (44.1 kHz stereo 16-bit, 480-sample blocks, as
   *  AirPlayTest encodes it). Played as a second track of the same player, so it keeps sync. */
  void pushAudio(const uint8_t* frame, size_t size, uint32_t samples);
  /** New mirror session: wait for the next keyframe, restart the timeline. */
  void newStream();
  /** Audio-only AirPlay (Music): the player runs with just the audio track, started by the first
   *  audio frame. A player left from a mirror session is rebuilt first. false on mirroring. */
  void setAudioOnly(bool on);
  bool active() const;
  /** Debug (GET :7000/msse?mime=...&annexb=0|1&config=0|1): change the sample format and
   *  recreate the player on the same surface. */
  std::string command(const std::string& query);
  /** JSON object: state, counters, last error, recent player events. */
  std::string statusJson() const;

  struct Impl;  // public only so the APMF listener classes in the .cpp can reach it

private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace ProbeNativeTurboModule
