#include <gtest/gtest.h>

#include "pychron/devices/spectrometer/roles.hpp"
#include "spectrometer/fakes.hpp"

namespace {

using namespace pychron;
using namespace pychron::spectrometer;

TEST(Range, ContainsAndClamp) {
  Limits l{-1.0, 2.0};
  EXPECT_TRUE(l.valid());
  EXPECT_TRUE(l.contains(-1.0));
  EXPECT_TRUE(l.contains(2.0));
  EXPECT_FALSE(l.contains(2.01));
  EXPECT_DOUBLE_EQ(l.clamp(5.0), 2.0);
  EXPECT_DOUBLE_EQ(l.clamp(-5.0), -1.0);
  EXPECT_DOUBLE_EQ(l.clamp(0.5), 0.5);
  EXPECT_FALSE((Range{1.0, 0.0}).valid());
}

TEST(Frame, ValueLookupByChannel) {
  Frame f;
  f.values = {{"H1", 1.5}, {"AX", 2.5}};
  EXPECT_EQ(f.value("AX"), 2.5);
  EXPECT_EQ(f.value("L1"), std::nullopt);
}

TEST(Caps, BitOperationsAndNames) {
  Caps none;
  EXPECT_TRUE(none.empty());
  EXPECT_EQ(to_string(none), "");

  Caps c = DetectorCap::Deflection | DetectorCap::Protect;
  EXPECT_TRUE(c.has(DetectorCap::Deflection));
  EXPECT_TRUE(c.has(DetectorCap::Protect));
  EXPECT_FALSE(c.has(DetectorCap::Gain));
  EXPECT_FALSE(c.has(DetectorCap::CddVoltage));
  EXPECT_EQ(to_string(c), "deflection|protect");
  EXPECT_EQ(c & Caps(DetectorCap::Protect), Caps(DetectorCap::Protect));
  EXPECT_EQ(to_string(DetectorCap::CddVoltage), "cdd_voltage");
}

TEST(Axis, NamesRoundTrip) {
  for (auto axis : {IMassPositioner::Axis::Dac, IMassPositioner::Axis::Field,
                    IMassPositioner::Axis::Mass}) {
    auto parsed = parse_axis(to_string(axis));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, axis);
  }
  auto bad = parse_axis("volts");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);
}

// Only the pure-virtual members; everything optional keeps its default.
struct MinimalPositioner final : IMassPositioner {
  Axis native_axis() const override { return Axis::Mass; }
  Result<void> set(double) override { return {}; }
  Result<double> read() override { return 0.0; }
  Limits limits() const override { return {0.0, 300.0}; }
};

struct MinimalAcquirer final : IIntensityAcquirer {
  std::vector<ChannelId> channels() const override { return {"H1"}; }
  bool integrates() const override { return true; }
  Result<void> configure(Duration) override { return {}; }
  Result<void> start() override { return {}; }
  Result<void> stop() override { return {}; }
  Result<std::optional<Frame>> next(Duration) override { return std::optional<Frame>{}; }
};

struct ProtectOnlyControl final : IDetectorControl {
  Caps caps() const override { return DetectorCap::Protect; }
  Result<void> protect(const ChannelId&, bool) override { return {}; }
};

TEST(RoleDefaults, PositionerIsNeverMovingByDefault) {
  MinimalPositioner p;
  auto moving = p.moving();
  ASSERT_TRUE(moving.has_value());
  EXPECT_FALSE(*moving);
}

TEST(RoleDefaults, TriggerIsANoOpForPolledBackends) {
  MinimalAcquirer a;
  EXPECT_TRUE(a.trigger().has_value());
}

TEST(RoleDefaults, UnimplementedDetectorOpsFailWithConfig) {
  ProtectOnlyControl dc;
  EXPECT_TRUE(dc.protect("H1", true).has_value());

  auto check = [](const auto& r, const std::string& what) {
    ASSERT_FALSE(r.has_value()) << what;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << what;
    EXPECT_NE(r.error().what.find(what), std::string::npos) << r.error().what;
  };
  check(dc.set_deflection("H1", 1.0), "deflection");
  check(dc.read_deflection("H1"), "deflection");
  check(dc.set_gain("H1", 1.0), "gain");
  check(dc.read_gain("H1"), "gain");
  check(dc.set_cdd_voltage("CDD", 1450.0), "cdd_voltage");
}

TEST(Composition, OneDriverCanImplementEveryRole) {
  // Integrated vendor shape: the same object answers for all five roles.
  struct Box final : IMassPositioner, IBeamSource, IIntensityAcquirer, IDetectorControl, IBeamBlank {
    Axis native_axis() const override { return Axis::Dac; }
    Result<void> set(double) override { return {}; }
    Result<double> read() override { return 0.0; }
    Limits limits() const override { return {0.0, 10.0}; }
    Result<void> set_hv(double) override { return {}; }
    Result<double> read_hv() override { return 4500.0; }
    std::span<const ParamSpec> params() const override { return {}; }
    Result<void> set_param(const ParamId&, double) override { return {}; }
    Result<Readback> read_param(const ParamId&) override { return Readback{}; }
    std::vector<ChannelId> channels() const override { return {"H1"}; }
    bool integrates() const override { return true; }
    Result<void> configure(Duration) override { return {}; }
    Result<void> start() override { return {}; }
    Result<void> stop() override { return {}; }
    Result<std::optional<Frame>> next(Duration) override { return std::optional<Frame>{}; }
    Caps caps() const override { return {}; }
    Result<void> protect(const ChannelId&, bool) override { return {}; }
    Result<void> blank(bool on) override {
      blanked = on;
      return {};
    }
    bool blanked = false;
  } box;

  IMassPositioner& positioner = box;
  IBeamSource& source = box;
  IIntensityAcquirer& acquirer = box;
  IBeamBlank& blanker = box;
  EXPECT_EQ(positioner.native_axis(), IMassPositioner::Axis::Dac);
  EXPECT_EQ(source.read_hv().value(), 4500.0);
  EXPECT_TRUE(acquirer.integrates());
  ASSERT_TRUE(blanker.blank(true).has_value());
  EXPECT_TRUE(box.blanked);
}

TEST(FakeAcquirer, IntegratedConfigureSnapsToLegalPeriod) {
  ManualClock clock;
  fakes::FakeAcquirer a{clock, true, {"H1"}};
  ASSERT_TRUE(a.configure(std::chrono::seconds(1)).has_value());
  EXPECT_EQ(a.period(), std::chrono::microseconds(131072 * 8));
}

}  // namespace
