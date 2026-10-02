// Role conformance suites for QtegraSpectrometer on the Qtegra sim hook.

#include <gtest/gtest.h>

#include "pychron/devices/spectrometer/thermo_qtegra.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "spectrometer/conformance.hpp"
#include "spectrometer/legacy/sim_util.hpp"

namespace {

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

// Shortest legal Qtegra integration period.
constexpr Duration kMinPeriod = std::chrono::nanoseconds(65'536'000);

struct QtegraHarness {
  ManualClock clock;
  std::shared_ptr<QtegraSimModel> model = [this] {
    auto m = std::make_shared<QtegraSimModel>();
    m->clock = &clock;
    m->move_time = kMinPeriod;
    m->integration_s = 0.065536;
    m->intensities = {{"H2", 0.1}, {"H1", 0.2}, {"AX", 0.3}, {"L1", 0.4}, {"L2", 0.5}, {"CDD", 0.6}};
    return m;
  }();
  std::unique_ptr<SimTransport> sim = open_hooked(qtegra_sim_hook(model));
  QtegraSpectrometer qtegra{"argus", *sim, {}, &clock};

  QtegraHarness() {
    EXPECT_TRUE(qtegra.connect());
    EXPECT_TRUE(qtegra.configure(kMinPeriod));
  }

  IMassPositioner& positioner() { return qtegra; }
  IIntensityAcquirer& acquirer() { return qtegra; }
  IBeamSource& source() { return qtegra; }
  IDetectorControl& detector_control() { return qtegra; }
  ChannelId channel() const { return "H1"; }
  // The suites reconfigure to longer periods; frames then take several steps.
  void advance() { clock.advance(kMinPeriod); }
  Duration timeout() const { return Duration::zero(); }
};

using Harnesses = ::testing::Types<QtegraHarness>;

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Qtegra, PositionerConformance, Harnesses);
INSTANTIATE_TYPED_TEST_SUITE_P(Qtegra, AcquirerConformance, Harnesses);
INSTANTIATE_TYPED_TEST_SUITE_P(Qtegra, SourceConformance, Harnesses);
INSTANTIATE_TYPED_TEST_SUITE_P(Qtegra, DetectorControlConformance, Harnesses);
