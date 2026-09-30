// Conformance suite instantiations for the legacy split-system drivers, each
// on a SimTransport hook that plays the hardware side.

#include <gtest/gtest.h>

#include <atomic>

#include "pychron/devices/spectrometer/legacy/adc_bank.hpp"
#include "pychron/devices/spectrometer/legacy/dac_positioner.hpp"
#include "pychron/devices/spectrometer/legacy/pulse_counter.hpp"
#include "pychron/devices/spectrometer/legacy/serial_hv.hpp"
#include "spectrometer/conformance.hpp"
#include "spectrometer/legacy/sim_util.hpp"

namespace {

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

struct DacPositionerHarness {
  std::unique_ptr<SimTransport> sim = open_hooked(map215_sim_hook({10.0, {}}));
  DacPositioner dac{"magnet_dac", std::make_unique<Map215Dac>(*sim, 0, 10.0), Limits{0.5, 9.5}};
  IMassPositioner& positioner() { return dac; }
  // Half an LSB of a 16-bit code over 10 V.
  double tolerance() const { return 1e-4; }
};

struct AdcBankHarness {
  ManualClock clock;
  std::unique_ptr<SimTransport> sim =
      open_hooked(adc_sim_hook({1, 0, [](std::size_t i) { return 0.1 * static_cast<double>(i + 1); }}));
  AdcBank adc{"faradays", *sim, {"AX", "H1", "L1"}, {100.0, 1, 0, 1.0}, &clock};
  IIntensityAcquirer& acquirer() { return adc; }
  void advance() { clock.advance(adc.sample_period()); }
};

struct PulseCounterHarness {
  ManualClock clock;
  std::atomic<std::uint64_t> n{0};
  std::unique_ptr<SimTransport> sim = open_hooked(counter_sim_hook({1, [this](std::size_t) { return ++n; }}));
  PulseCounter counter{"multiplier", *sim, {"EM"}, 10.0, &clock};
  IIntensityAcquirer& acquirer() { return counter; }
  void advance() { clock.advance(counter.sample_period()); }
};

struct SerialHvHarness {
  std::unique_ptr<SimTransport> sim = open_hooked(hv_sim_hook({0.0, 10000.0, {}, {}}));
  SerialHv hv{"spellman", *sim, 10000.0};
  IBeamSource& source() { return hv; }
  // The supply reports one decimal place.
  double tolerance() const { return 1e-4; }
};

using Positioners = ::testing::Types<DacPositionerHarness>;
using Acquirers = ::testing::Types<AdcBankHarness, PulseCounterHarness>;
using Sources = ::testing::Types<SerialHvHarness>;

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Legacy, PositionerConformance, Positioners);
INSTANTIATE_TYPED_TEST_SUITE_P(Legacy, AcquirerConformance, Acquirers);
INSTANTIATE_TYPED_TEST_SUITE_P(Legacy, SourceConformance, Sources);
