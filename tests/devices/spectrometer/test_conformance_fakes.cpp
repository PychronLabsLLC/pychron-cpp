// Instantiates the conformance suites for the in-memory fakes: proves the
// suites themselves pass for conforming drivers in both composition shapes.

#include "spectrometer/conformance.hpp"
#include "spectrometer/fakes.hpp"

namespace {

using namespace pychron;
using namespace pychron::spectrometer;

struct DacHarness {
  fakes::FakeDac dac;
  IMassPositioner& positioner() { return dac; }
};

struct MagnetHarness {
  fakes::FakeMagnet magnet{4};
  IMassPositioner& positioner() { return magnet; }
};

struct AdcBankHarness {
  ManualClock clock;
  fakes::FakeAcquirer adc{clock, false, {"AX", "H1", "L1"}};
  IIntensityAcquirer& acquirer() { return adc; }
  void advance() { clock.advance(std::chrono::milliseconds(10)); }
};

struct IntegratedHarness {
  ManualClock clock;
  fakes::FakeAcquirer box{clock, true, {"H2", "H1", "AX", "L1", "L2", "CDD"}};
  IIntensityAcquirer& acquirer() { return box; }
  void advance() { clock.advance(std::chrono::seconds(1)); }
};

struct HvOnlyHarness {
  fakes::FakeSource src = fakes::FakeSource::hv_only();
  IBeamSource& source() { return src; }
};

struct VendorSourceHarness {
  fakes::FakeSource src = fakes::FakeSource::vendor();
  IBeamSource& source() { return src; }
};

struct FullControlHarness {
  fakes::FakeDetectorControl dc{DetectorCap::Deflection | DetectorCap::Protect};
  IDetectorControl& detector_control() { return dc; }
  ChannelId channel() const { return "H1"; }
};

struct NoControlHarness {
  fakes::FakeDetectorControl dc{Caps{}};
  IDetectorControl& detector_control() { return dc; }
  ChannelId channel() const { return "H1"; }
};

using Positioners = ::testing::Types<DacHarness, MagnetHarness>;
using Acquirers = ::testing::Types<AdcBankHarness, IntegratedHarness>;
using Sources = ::testing::Types<HvOnlyHarness, VendorSourceHarness>;
using DetectorControls = ::testing::Types<FullControlHarness, NoControlHarness>;

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Fakes, PositionerConformance, Positioners);
INSTANTIATE_TYPED_TEST_SUITE_P(Fakes, AcquirerConformance, Acquirers);
INSTANTIATE_TYPED_TEST_SUITE_P(Fakes, SourceConformance, Sources);
INSTANTIATE_TYPED_TEST_SUITE_P(Fakes, DetectorControlConformance, DetectorControls);
