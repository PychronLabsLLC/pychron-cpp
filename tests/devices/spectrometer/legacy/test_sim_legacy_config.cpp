// configs/examples/spectrometer.sim-legacy.toml drives the real legacy
// drivers end to end: every [drivers.*] table (its sim_* kind swapped for the
// hardware kind) is built through the DriverRegistry on a SimTransport whose
// hook plays that transport's hardware, all sharing one toy beam. The config
// then positions the magnet, sets HV, and acquires from both acquirers.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <mutex>

#include <toml++/toml.hpp>

#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/legacy/adc_bank.hpp"
#include "pychron/devices/spectrometer/legacy/dac_positioner.hpp"
#include "pychron/devices/spectrometer/legacy/pulse_counter.hpp"
#include "pychron/devices/spectrometer/legacy/serial_hv.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;
using namespace std::chrono_literals;

namespace {

const std::string kConfig = std::string(PYCHRON_EXAMPLES_DIR) + "/spectrometer.sim-legacy.toml";

// Hardware kind behind each simulated driver kind in the example config.
const std::map<std::string, std::string> kHardwareKind{{"sim_dac_positioner", "dac_positioner"},
                                                       {"sim_adc_bank", "adc_bank"},
                                                       {"sim_pulse_counter", "pulse_counter"},
                                                       {"sim_hv_supply", "serial_hv"}};

// Faraday peaks on the DAC axis: channel i centred at kCentre + (i - 1) * kSpacing,
// amplitude proportional to HV.
constexpr double kCentre = 5.0;
constexpr double kSpacing = 0.5;
constexpr double kWidth = 0.05;
constexpr double kPeakVolts = 2.0;
constexpr std::uint64_t kCountsPerSample = 40;

struct ToyBeam {
  std::mutex mutex;
  double dac = 0.0;
  double hv = 0.0;
  double nominal_hv = 4500.0;

  double faraday(std::size_t channel) {
    std::lock_guard lock(mutex);
    const double centre = kCentre + (static_cast<double>(channel) - 1.0) * kSpacing;
    const double z = (dac - centre) / kWidth;
    return kPeakVolts * (hv / nominal_hv) * std::exp(-0.5 * z * z);
  }
};

struct LegacyRig {
  toml::table config;
  ManualClock clock;
  ToyBeam beam;
  std::map<std::string, std::unique_ptr<SimTransport>> transports;
  std::map<std::string, std::unique_ptr<Device>> drivers;

  SimTransport::Hook hook_for(const std::string& hardware_kind, const toml::table& options) {
    if (hardware_kind == "dac_positioner") {
      return map215_sim_hook({options["full_scale"].value_or(10.0), [this](int, double v) {
                                std::lock_guard lock(beam.mutex);
                                beam.dac = v;
                              }});
    }
    if (hardware_kind == "adc_bank") {
      return adc_sim_hook({1, 0, [this](std::size_t i) { return beam.faraday(i); }});
    }
    if (hardware_kind == "pulse_counter") {
      const auto n = options["channels"].as_array()->size();
      return counter_sim_hook({n, [](std::size_t) { return kCountsPerSample; }});
    }
    return hv_sim_hook({0.0, 10000.0, [this](double s) {
                          std::lock_guard lock(beam.mutex);
                          beam.hv = s;
                        },
                        {}});
  }

  void build() {
    auto parsed = toml::parse_file(kConfig);
    ASSERT_TRUE(parsed) << parsed.error().description();
    config = std::move(parsed).table();
    beam.nominal_hv = config["source"]["nominal_hv"].value_or(4500.0);

    const auto* drivers_table = config["drivers"].as_table();
    ASSERT_NE(drivers_table, nullptr);
    for (const auto& [key, node] : *drivers_table) {
      const std::string name(key.str());
      toml::table options = *node.as_table();
      const std::string sim_kind = options["kind"].value_or(std::string());
      ASSERT_TRUE(kHardwareKind.contains(sim_kind)) << name << ": " << sim_kind;
      const std::string& kind = kHardwareKind.at(sim_kind);
      const std::string transport = options["transport"].value_or(std::string());
      ASSERT_TRUE(config["transports"][transport].is_table()) << name << " -> " << transport;
      options.erase("kind");
      options.erase("transport");

      auto sim = open_hooked(hook_for(kind, options));
      auto dev = DriverRegistry::global().create(kind, *sim, options, DriverContext{name, &clock});
      ASSERT_TRUE(dev) << name << ": " << to_string(dev.error());
      transports[name] = std::move(sim);
      drivers[name] = std::move(*dev);
    }
  }

  template <class Role>
  Role& role(const std::string& driver) {
    auto* r = dynamic_cast<Role*>(drivers.at(driver).get());
    EXPECT_NE(r, nullptr) << driver;
    return *r;
  }

  IMassPositioner& positioner() { return role<IMassPositioner>(config["magnet"]["positioner"].value_or(std::string())); }
  IBeamSource& source() { return role<IBeamSource>(config["source"]["driver"].value_or(std::string())); }

  std::vector<std::string> acquirer_names() {
    std::vector<std::string> names;
    for (const auto& n : *config["acquisition"]["acquirers"].as_array()) names.push_back(*n.value<std::string>());
    return names;
  }

  // Host-side mean per channel over `n` raw frames from every acquirer; the
  // shared ManualClock steps by the fastest sample period.
  std::map<std::string, double> acquire(int n) {
    std::vector<PolledAcquirer*> acquirers;
    for (const auto& name : acquirer_names()) {
      auto* a = dynamic_cast<PolledAcquirer*>(drivers.at(name).get());
      EXPECT_NE(a, nullptr) << name;
      if (a == nullptr) return {};
      EXPECT_TRUE(a->configure(1s));
      EXPECT_TRUE(a->start());
      acquirers.push_back(a);
    }
    std::map<std::string, double> sums;
    std::map<std::string, int> counts;
    for (auto* a : acquirers) {
      const std::string& prefix = a->name();
      std::uint64_t last_seq = 0;
      for (int got = 0, polls = 0; got < n && polls < 10 * n; ++polls) {
        auto f = a->next(1s);
        EXPECT_TRUE(f) << (f ? "" : to_string(f.error()));
        if (!f) break;
        clock.advance(a->sample_period());
        if (!*f) continue;
        EXPECT_GT((*f)->seq, last_seq);
        last_seq = (*f)->seq;
        EXPECT_FALSE((*f)->integrated);
        for (const auto& [channel, value] : (*f)->values) {
          sums[prefix + ":" + channel] += value;
          ++counts[prefix + ":" + channel];
        }
        ++got;
      }
      EXPECT_TRUE(a->stop());
    }
    for (auto& [channel, sum] : sums) {
      EXPECT_EQ(counts[channel], n) << channel;
      sum /= counts[channel];
    }
    return sums;
  }
};

}  // namespace

TEST(SimLegacyConfig, EveryDriverBuildsAndPlaysItsConfiguredRoles) {
  LegacyRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.build());
  ASSERT_EQ(rig.drivers.size(), 4U);
  for (const auto& [name, node] : *rig.config["drivers"].as_table()) {
    for (const auto& r : *node.as_table()->get_as<toml::array>("roles")) {
      const std::string role = *r.value<std::string>();
      Device* dev = rig.drivers.at(std::string(name.str())).get();
      if (role == "positioner") {
        EXPECT_NE(dynamic_cast<IMassPositioner*>(dev), nullptr) << name;
      }
      if (role == "acquirer") {
        EXPECT_NE(dynamic_cast<IIntensityAcquirer*>(dev), nullptr) << name;
      }
      if (role == "source") {
        EXPECT_NE(dynamic_cast<IBeamSource*>(dev), nullptr) << name;
      }
    }
  }
  EXPECT_EQ(rig.positioner().native_axis(), IMassPositioner::Axis::Dac);
}

TEST(SimLegacyConfig, EachDetectorBindsToExactlyOneAcquirerChannel) {
  LegacyRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.build());
  std::map<std::string, int> bound;
  for (const auto& name : rig.acquirer_names()) {
    for (const auto& ch : rig.role<IIntensityAcquirer>(name).channels()) bound[name + ":" + ch] = 0;
  }
  for (const auto& d : *rig.config["detectors"].as_array()) {
    const std::string channel = d.as_table()->get_as<std::string>("channel")->get();
    ASSERT_TRUE(bound.contains(channel)) << channel;
    ++bound[channel];
  }
  for (const auto& [channel, n] : bound) EXPECT_EQ(n, 1) << channel;
}

TEST(SimLegacyConfig, PositionSetHvAcquireEndToEnd) {
  LegacyRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.build());

  auto& source = rig.source();
  ASSERT_TRUE(source.set_hv(rig.beam.nominal_hv));
  auto hv = source.read_hv();
  ASSERT_TRUE(hv);
  EXPECT_DOUBLE_EQ(*hv, rig.beam.nominal_hv);

  // Centre H1 (faradays channel index 1) on the magnet.
  auto& magnet = rig.positioner();
  const auto limits = magnet.limits();
  EXPECT_EQ(limits, (Limits{rig.config["magnet"]["limits"]["min"].value_or(0.0),
                            rig.config["magnet"]["limits"]["max"].value_or(10.0)}));
  ASSERT_TRUE(magnet.set(kCentre));
  EXPECT_NEAR(*magnet.read(), kCentre, 1e-3);

  auto means = rig.acquire(10);
  EXPECT_NEAR(means["faradays:H1"], kPeakVolts, 1e-3);
  EXPECT_LT(means["faradays:AX"], 1e-6);
  EXPECT_LT(means["faradays:L1"], 1e-6);
  EXPECT_DOUBLE_EQ(means["multiplier:EM"], static_cast<double>(kCountsPerSample));

  // Half the HV halves the Faraday signal.
  ASSERT_TRUE(source.set_hv(rig.beam.nominal_hv / 2));
  means = rig.acquire(10);
  EXPECT_NEAR(means["faradays:H1"], kPeakVolts / 2, 1e-3);
}

TEST(SimLegacyConfig, DacScanFindsEachPeak) {
  LegacyRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.build());
  ASSERT_TRUE(rig.source().set_hv(rig.beam.nominal_hv));
  auto& magnet = rig.positioner();

  std::map<std::string, std::pair<double, double>> best;  // channel -> {signal, dac}
  for (double dac = 4.0; dac <= 6.0 + 1e-9; dac += 0.01) {
    ASSERT_TRUE(magnet.set(dac));
    for (const auto& [channel, v] : rig.acquire(1)) {
      if (channel.starts_with("faradays:") && v > best[channel].first) best[channel] = {v, *magnet.read()};
    }
  }
  EXPECT_NEAR(best["faradays:AX"].second, kCentre - kSpacing, 0.01);
  EXPECT_NEAR(best["faradays:H1"].second, kCentre, 0.01);
  EXPECT_NEAR(best["faradays:L1"].second, kCentre + kSpacing, 0.01);
}
