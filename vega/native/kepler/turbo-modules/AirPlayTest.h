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
  using Advertise = std::function<void(int port, const std::map<std::string, std::string>& txt)>;
  /** Receives fMP4 bytes (init segments and fragments) for the JS MSE player. */
  using Media = std::function<void(std::vector<uint8_t>&& bytes)>;

  /** media: video fMP4 (H.264); audioMedia: audio fMP4 (FLAC), played by a separate player. */
  AirPlayTest(Log log, Advertise advertise, Media media, Media audioMedia);
  ~AirPlayTest();

  /** Starts the server. Returns a one-line status. Safe to call twice. */
  std::string start(const std::string& deviceName, const std::string& deviceId);
  /** JSON fragment with counters, e.g. {"state":...,"video":{...},"audio":{...}}. */
  std::string statusJson() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ProbeNativeTurboModule
