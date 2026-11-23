#include "ProbeNativeSpec.h"

using namespace com::amazon::kepler::turbomodule;

namespace ProbeNativeTurboModule {

ProbeNativeSpec::ProbeNativeSpec() : KeplerTurboModule("ProbeNative") {}

ProbeNativeSpec::~ProbeNativeSpec() = default;

void ProbeNativeSpec::aggregateMethods(MethodAggregator<KeplerTurboModule>& methodAggregator) const noexcept {
  methodAggregator.addMethod("start", &ProbeNativeSpec::start);
  methodAggregator.addMethod("status", &ProbeNativeSpec::status);
  methodAggregator.addMethod("stop", &ProbeNativeSpec::stop);
  methodAggregator.addMethod("readVideo", &ProbeNativeSpec::readVideo);
  methodAggregator.addMethod("readAudio", &ProbeNativeSpec::readAudio);
  methodAggregator.addMethod("report", &ProbeNativeSpec::report);
  methodAggregator.addMethod("airplayStart", &ProbeNativeSpec::airplayStart);
}

} // namespace ProbeNativeTurboModule
