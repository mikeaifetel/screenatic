#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace ProbeNativeTurboModule {

/**
 * Stage 0 of vega/PORTING.md: runs the real AirPlay core (UxPlay via the Android build's
 * airplay_server.cpp) on Vega, plus the audio path's codecs (FFmpeg AAC-ELD decoder, libFLAC
 * encoder). Video frames are only counted; audio is decoded and FLAC-encoded and the CPU time
 * spent is reported, which tells us whether the stick can afford the AAC-ELD -> FLAC conversion.
 */
class AirPlayTest {
public:
  using Log = std::function<void(const std::string&)>;
  /** Called after start with the port and TXT record to advertise as _airplay._tcp. */
  using Advertise = std::function<void(int port, const std::map<std::string, std::string>& txt)>;

  AirPlayTest(Log log, Advertise advertise);
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
