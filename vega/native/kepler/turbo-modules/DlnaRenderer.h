#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ProbeNativeTurboModule {

/**
 * DLNA / UPnP MediaRenderer (DMR): Android phones' "cast to" (BubbleUPnP, gallery and video apps),
 * Windows "Cast to Device", NAS apps. The controller hands us a URL and drives play/pause/seek/volume;
 * playback itself is the JS player, driven by events in the same queue as AirPlay's (pollEvents).
 *
 * Port of the Android build's protocol-dlna (DlnaEngine, UpnpXml, Scpd, Gena, MiniHttpServer).
 * Hand-rolled HTTP/SOAP + GENA (no UPnP library). SSDP and the TCP 49595 listener live in
 * ProbeNative, which hands each accepted connection to serve().
 */
class DlnaRenderer {
public:
  using Log = std::function<void(const std::string&)>;
  /** One JSON event for the JS side, e.g. {"type":"play","source":"dlna",...}. */
  using Emit = std::function<void(const std::string&)>;

  static const char* deviceType();
  static std::vector<std::string> serviceTypes();

  DlnaRenderer();
  ~DlnaRenderer();

  /** Before the first start(); not changed afterwards. */
  void setCallbacks(Log log, Emit emit);
  /** Starts the eventing and timer threads. Safe to call twice. */
  void start(const std::string& friendlyName, const std::string& uuid);
  void stop();

  /** One HTTP connection accepted on :49595 (description, SCPD, SOAP control, GENA). Closes fd. */
  void serve(int fd, const std::string& remoteIp);

  /** Player state from JS (seconds). closed: the player went away for another reason than a
   *  DLNA Stop (another sender took over, the user pressed Back). */
  void reportPlayback(double durationSec, double positionSec, double rate, bool ready, bool buffering,
                      bool ended, bool failed, bool closed);

  /** Debug: {"transport":...,"uri":...,"controller":...,"subscriptions":[...]} for GET /status. */
  std::string statusJson();

private:
  struct Impl;
  std::shared_ptr<Impl> d_;
};

}  // namespace ProbeNativeTurboModule
