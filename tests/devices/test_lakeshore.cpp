// lakeshore over its simulator (plan 2026-10-05, tasks C2/C3).
#include "pychron/devices/lakeshore.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/devices/driver_registry.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions fast() {
  TransportOptions o;
  o.name = "cryostat";
  o.timeout = 50ms;
  return o;
}

// The ldeo-style bands: range 1 below 10 K, 2 to 30 K, 3 above (legacy's
// defaults, without their gaps), output 1 only.
constexpr const char* kBands = R"(
ranges = [
  { output = 1, range = 1, min = 0.0, max = 10.0 },
  { output = 1, range = 2, min = 10.0, max = 30.0 },
  { output = 1, range = 3, min = 30.0, max = 400.0 },
]
)";

struct Rig {
  explicit Rig(std::string toml = kBands) {
    EXPECT_TRUE(bus->open());
    auto parsed = toml::parse(toml);
    auto made = DriverRegistry::global().create("lakeshore", *bus, parsed.table(), DriverContext{"cryo", &clock});
    EXPECT_TRUE(made) << made.error().what;
    if (made) device = std::move(*made);
  }
  ITemperatureController& tc() { return *capability<ITemperatureController>(*device); }
  std::vector<std::string> sent() const {
    std::vector<std::string> out;
    for (const auto& tx : bus->written()) out.push_back(to_string(tx));
    return out;
  }

  ManualClock clock;
  LakeshoreSim sim{clock};
  std::unique_ptr<SimTransport> bus = SimTransport::hooked(sim.hook(), fast());
  std::unique_ptr<Device> device;
};

}  // namespace

TEST(Lakeshore, ConnectChecksItIsTheConfiguredModel) {
  Rig rig;
  ASSERT_TRUE(capability<IConnectable>(*rig.device)->connect());
  EXPECT_EQ(rig.sent().front(), "*CLS\n");
  Rig wrong("model = \"336\"");
  auto r = capability<IConnectable>(*wrong.device)->connect();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("MODEL335"), std::string::npos) << r.error().what;
}

TEST(Lakeshore, ReadsInputsInKelvin) {
  Rig rig;
  rig.sim.set_temperature('A', 77.5);
  EXPECT_DOUBLE_EQ(*rig.tc().read_temperature("A"), 77.5);
  EXPECT_EQ(rig.sent().back(), "KRDG? A\n");
  EXPECT_EQ(rig.tc().inputs(), (std::vector<std::string>{"A", "B"}));
  auto unknown = rig.tc().read_temperature("C");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
}

TEST(Lakeshore, ACelsiusUnitIsConvertedBothWays) {
  Rig rig("units = \"C\"");
  rig.sim.set_temperature('A', 293.15);
  EXPECT_NEAR(*rig.tc().read_temperature("A"), 293.15, 1e-3);
  EXPECT_EQ(rig.sent().back(), "CRDG? A\n");
  ASSERT_TRUE(rig.tc().set_setpoint(1, 300.0));
  EXPECT_NEAR(*rig.sim.setpoint(1), 26.85, 1e-3);  // the unit was told Celsius
  EXPECT_NEAR(*rig.tc().setpoint(1), 300.0, 1e-3);
}

TEST(Lakeshore, ASetpointPicksItsBandSetsAndReadsBack) {
  Rig rig;
  ASSERT_TRUE(rig.tc().set_setpoint(1, 14.0));
  EXPECT_EQ(rig.sim.range(1), 2);
  EXPECT_DOUBLE_EQ(*rig.sim.setpoint(1), 14.0);
  const auto sent = rig.sent();
  EXPECT_EQ(std::vector<std::string>(sent.end() - 3, sent.end()),
            (std::vector<std::string>{"RANGE 1,2\n", "SETP 1,14.000\n", "SETP? 1\n"}));
}

TEST(Lakeshore, BandBoundariesAreCovered) {
  // Legacy's default bands (v<10, 10<v<30, v>30) left 10 and 30 in none.
  Rig rig;
  for (auto [k, range] : {std::pair{10.0, 2}, {30.0, 3}, {9.999, 1}, {0.0, 1}, {400.0, 3}}) {
    ASSERT_TRUE(rig.tc().set_setpoint(1, k)) << k;
    EXPECT_EQ(rig.sim.range(1), range) << k;
  }
  auto outside = rig.tc().set_setpoint(1, 401.0);
  ASSERT_FALSE(outside);
  EXPECT_EQ(outside.error().kind, ErrorKind::Config);
}

TEST(Lakeshore, RangeZeroIsSelectable) {
  Rig rig(R"(ranges = [ { output = 1, range = 0, min = 290.0, max = 300.0 } ])");
  ASSERT_TRUE(rig.tc().set_setpoint(1, 1.0 + 293.0));
  bool sent_range_zero = false;
  for (const auto& s : rig.sent()) sent_range_zero |= s == "RANGE 1,0\n";
  EXPECT_TRUE(sent_range_zero);
}

TEST(Lakeshore, WithoutBandsTheRangeIsLeftAlone) {
  Rig rig("");
  ASSERT_TRUE(rig.tc().set_setpoint(2, 120.0));
  for (const auto& s : rig.sent()) EXPECT_FALSE(s.starts_with("RANGE")) << s;
}

TEST(Lakeshore, SameSetpointTwiceIsSentTwice) {
  // Legacy's trait handler skipped a repeat; a unit reset in between would
  // have kept the wrong one.
  Rig rig;
  ASSERT_TRUE(rig.tc().set_setpoint(1, 77.0));
  ASSERT_TRUE(rig.tc().set_setpoint(1, 77.0));
  int setps = 0;
  for (const auto& s : rig.sent()) setps += s == "SETP 1,77.000\n";
  EXPECT_EQ(setps, 2);
}

TEST(Lakeshore, VerifyUsesTolerance) {
  Rig rig;
  rig.sim.set_setpoint_error(0.005);  // within the 0.01 K default
  EXPECT_TRUE(rig.tc().set_setpoint(1, 77.0));
  rig.sim.set_setpoint_error(0.5);
  auto r = rig.tc().set_setpoint(1, 77.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("read back"), std::string::npos);
  int setps = 0;
  for (const auto& s : rig.sent()) setps += s == "SETP 1,77.000\n";
  EXPECT_EQ(setps, 1 + 4);  // the first set, then 1 + 3 retries
}

TEST(Lakeshore, ConfigIsChecked) {
  auto bad = [](std::string toml) {
    ManualClock clock;
    auto bus = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, fast());
    auto parsed = toml::parse(toml);
    return !DriverRegistry::global().create("lakeshore", *bus, parsed.table(), DriverContext{"x", &clock});
  };
  EXPECT_TRUE(bad("model = \"340\""));
  EXPECT_TRUE(bad("inputs = [\"C\"]"));  // a 335 has A and B
  EXPECT_FALSE(bad("model = \"336\"\ninputs = [\"C\", \"D\"]"));
  EXPECT_TRUE(bad("inputs = [\"A\", \"A\"]"));
  EXPECT_TRUE(bad("units = \"F\""));
  EXPECT_TRUE(bad("ranges = [ { output = 3, range = 1, min = 0.0, max = 1.0 } ]"));  // a 335 has 2 outputs
  EXPECT_TRUE(bad("ranges = [ { output = 1, range = 1, min = 5.0, max = 1.0 } ]"));
  EXPECT_TRUE(bad("ranges = [ { output = 1, range = 1, min = 0.0, max = 20.0 }, { output = 1, range = 2, min = 10.0, max = "
                  "30.0 } ]"));  // overlap
  EXPECT_TRUE(bad("ranges = [\"x\"]"));
}

TEST(Lakeshore, TheSimulatorFollowsTheSetpointWhileHeating) {
  Rig rig;
  rig.sim.set_temperature('A', 293.15);
  ASSERT_TRUE(rig.tc().set_setpoint(1, 77.0));  // range 3: heating
  rig.clock.advance(10min);
  EXPECT_NEAR(*rig.tc().read_temperature("A"), 77.0, 0.02);  // replies carry three decimals
}
