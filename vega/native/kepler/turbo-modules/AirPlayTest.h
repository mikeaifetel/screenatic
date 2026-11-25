#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ProbeNativeTurboModule {

/**
 * Stage 0 of vega/PORTING.md: runs the real AirPlay core (UxPlay via the Android build's
 * airplay_server.cpp) on Vega, plus the audio path's codecs (FFmpeg AAC-ELD decoder, libFLAC
 * encoder). Video frames are only counted; audio is decoded and FLAC-encoded and the CPU time
 * spent is reported, which tells us whether the stick can afford the AAC-ELD -> FLAC conversion.
 * Stage 1: mirrored H.264 is muxed into fMP4 (Fmp4Muxer) and handed to JS for MSE playback.
 */
class AirPlayTest {
public:
  using Log = std::function<void(const std::string&)>;
  /** Called after start with the port and TXT record to advertise as _airplay._tcp. */
  using Advertise = std::function<void(int port, const std::map<std::string, std::string>& airplayTxt,
                                       const std::map<std::string, std::string>& raopTxt)>;
  /** Receives fMP4 bytes (init segments and fragments) for the JS MSE player. */
  using Media = std::function<void(std::vector<uint8_t>&& bytes)>;

  /** media: video fMP4 (H.264); audioMedia: audio fMP4 (FLAC), played by a separate player. */
  AirPlayTest(Log log, Advertise advertise, Media media, Media audioMedia);
  ~AirPlayTest();

  /** Starts the server. Returns a one-line status. Safe to call twice. */
  /** lanIp: the TV's address, used in the HLS URLs handed to Vega's player (localhost there is
   *  not this app). */
  /** debug: request tracing (timeline) and session dumps for GET :7000/status and /dump.*. */
  std::string start(const std::string& deviceName, const std::string& deviceId, const std::string& lanIp,
                    bool debug);
  /** JSON fragment with counters, e.g. {"state":...,"video":{...},"audio":{...}}. */
  std::string statusJson() const;

  // ---- AirPlay video (URL mode): the sender hands over a media URL instead of mirroring ----
  /** JSON array of events since the last call: {"type":"play","url":...,"startMs":...},
   *  {"type":"cmd","cmd":0 seek|1 rate|2 stop,"value":...}, {"type":"disconnected"},
   *  {"type":"mirror"} (mirroring started), {"type":"size","width":...,"height":...} (mirror
   *  picture size), {"type":"volume","gain":...}. */
  std::string pollEvents();
  /** Debug: queue a play event as if a sender had sent this URL (GET /play?url=... on :7000). */
  void debugPlay(const std::string& url);
  /** Debug: one H.264 access unit (Annex-B) fed as if it came from a mirroring sender, so a
   *  recorded or synthetic stream can be replayed through the muxer (TCP :7100, "SCRV" framing).
   *  first: start of a new stream (muxer reset). */
  void debugVideo(const uint8_t* data, size_t size, int64_t ptsUs, bool first);
  /** Play the mirror's video natively (MssePlayer, MSSE mode) on this JS surface handle. */
  std::string msseAttach(const std::string& windowHandle);
  /** Debug: GET :7000/msse?... (see MssePlayer::command). */
  std::string msseCommand(const std::string& query);
  /** Debug: the first ~20 MB of the current mirror session. kind: "h264" (raw Annex-B access
   *  units), "mp4" (what MSE was fed), "pts" (one line per frame: ptsUs size key). */
  std::string dump(const std::string& kind);
  /** Player state for the sender's ~1 Hz /playback-info polls. */
  void setPlayback(double durationSec, double positionSec, double rate, bool ready, bool buffering,
                   bool finished);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ProbeNativeTurboModule
