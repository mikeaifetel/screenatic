#pragma once

#include "generated/ProbeNativeSpec.h"

#include <memory>

namespace ProbeNativeTurboModule {

class Probe;
class AirPlayTest;

class ProbeNative : public ProbeNativeSpec {
public:
  ProbeNative();
  ~ProbeNative() noexcept;

  std::string start(std::string deviceName) override;
  std::string status() override;
  void stop() override;
  com::amazon::kepler::turbomodule::ArrayBuffer readVideo(double maxBytes) override;
  com::amazon::kepler::turbomodule::ArrayBuffer readAudio(double maxBytes) override;
  void report(std::string section, std::string json) override;
  std::string pollEvents() override;
  void setPlayback(std::string csv) override;
  std::string msseAttach(std::string windowHandle) override;
  std::string airplayStart(std::string deviceName) override;

private:
  std::shared_ptr<Probe> probe_;
  std::unique_ptr<AirPlayTest> airplay_;
};

} // namespace ProbeNativeTurboModule
