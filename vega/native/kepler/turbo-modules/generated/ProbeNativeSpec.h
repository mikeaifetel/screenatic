#pragma once

#include <Kepler/turbomodule/ArrayBuffer.h>
#include <Kepler/turbomodule/Callback.h>
#include <Kepler/turbomodule/Error.h>
#include <Kepler/turbomodule/HostCallback.h>
#include <Kepler/turbomodule/JSValue.h>
#include <Kepler/turbomodule/KeplerTurboModule.h>
#include <Kepler/turbomodule/NativeObject.h>
#include <Kepler/turbomodule/Promise.h>
#include <Kepler/turbomodule/TMLog.h>

#include <string>
#include <vector>

namespace ProbeNativeTurboModule {

class ProbeNativeSpec : public com::amazon::kepler::turbomodule::KeplerTurboModule {
protected:
  ProbeNativeSpec();
  virtual ~ProbeNativeSpec() = 0;

public:
  void aggregateMethods(com::amazon::kepler::turbomodule::MethodAggregator<KeplerTurboModule>& methodAggregator) const noexcept override;

  virtual std::string start(std::string deviceName) = 0;
  virtual std::string status() = 0;
  virtual void stop() = 0;
  virtual com::amazon::kepler::turbomodule::ArrayBuffer readVideo(double maxBytes) = 0;
  virtual com::amazon::kepler::turbomodule::ArrayBuffer readAudio(double maxBytes) = 0;
  virtual void report(std::string section, std::string json) = 0;
  virtual std::string pollEvents() = 0;
  virtual void setPlayback(std::string csv) = 0;
  virtual std::string msseAttach(std::string windowHandle) = 0;
  virtual std::string airplayStart(std::string deviceName) = 0;
  virtual std::string startReceiver(std::string deviceName, bool debug) = 0;
};

} // namespace ProbeNativeTurboModule
