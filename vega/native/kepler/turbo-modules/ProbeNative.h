#pragma once

#include "generated/ProbeNativeSpec.h"

#include <memory>

namespace ProbeNativeTurboModule {

class Probe;

class ProbeNative : public ProbeNativeSpec {
public:
  ProbeNative();
  ~ProbeNative() noexcept;

  std::string start(std::string deviceName) override;
  std::string status() override;
  void stop() override;
  com::amazon::kepler::turbomodule::ArrayBuffer readVideo(double maxBytes) override;

private:
  std::shared_ptr<Probe> probe_;
};

} // namespace ProbeNativeTurboModule
