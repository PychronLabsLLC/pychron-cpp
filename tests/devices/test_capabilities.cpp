#include "pychron/devices/capabilities.hpp"

#include <gtest/gtest.h>

#include <map>

#include "pychron/devices/device.hpp"

using namespace pychron;

namespace {

// A vendor-free fake that implements every capability, as a driver would.
class FakeLine : public Device, public IPressureGauge, public IValveActuator, public IScannable {
 public:
  explicit FakeLine(std::string name) : Device(std::move(name)) {}

  Result<double> read_pressure() override { return observe(Result<double>(1e-8)); }

  Result<void> open(const ValveAddress& a) override {
    states_[a] = ValveState::Open;
    return {};
  }
  Result<void> close(const ValveAddress& a) override {
    states_[a] = ValveState::Closed;
    return {};
  }
  Result<ValveState> read(const ValveAddress& a) override {
    auto it = states_.find(a);
    if (it == states_.end()) return fail(ErrorKind::Protocol, "no such address " + a.value, name());
    return it->second;
  }

  Result<Sample> sample() override {
    auto p = read_pressure();
    if (!p) return fail(p.error());
    return Sample{name(), TimePoint{}, *p};
  }

 private:
  std::map<ValveAddress, ValveState> states_;
};

class GaugeOnly : public Device, public IPressureGauge {
 public:
  GaugeOnly() : Device("g") {}
  Result<double> read_pressure() override { return 2.0; }
};

}  // namespace

TEST(Capabilities, ManagersUseInterfacesOnly) {
  FakeLine line("line");
  IValveActuator& act = line;
  ValveAddress a{"1"};
  ASSERT_TRUE(act.open(a));
  EXPECT_EQ(*act.read(a), ValveState::Open);
  ASSERT_TRUE(act.close(a));
  EXPECT_EQ(*act.read(a), ValveState::Closed);
  auto missing = act.read(ValveAddress{"9"});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Protocol);

  IScannable& scan = line;
  auto s = scan.sample();
  ASSERT_TRUE(s);
  EXPECT_EQ(s->device, "line");
  EXPECT_DOUBLE_EQ(s->value, 1e-8);
}

TEST(Capabilities, CapabilityLookupFromDevice) {
  FakeLine line("line");
  Device& d = line;
  EXPECT_NE(capability<IPressureGauge>(d), nullptr);
  EXPECT_NE(capability<IValveActuator>(d), nullptr);
  EXPECT_NE(capability<IScannable>(d), nullptr);

  GaugeOnly g;
  Device& dg = g;
  ASSERT_NE(capability<IPressureGauge>(dg), nullptr);
  EXPECT_DOUBLE_EQ(*capability<IPressureGauge>(dg)->read_pressure(), 2.0);
  EXPECT_EQ(capability<IValveActuator>(dg), nullptr);
  EXPECT_EQ(capability<IScannable>(dg), nullptr);
}

TEST(Capabilities, ValveAddressAsIndex) {
  EXPECT_EQ(*ValveAddress{"12"}.as_index(), 12);
  EXPECT_EQ(*ValveAddress{" 3 "}.as_index(), 3);
  for (const char* bad : {"", "A", "1a", "-1", "99999999999999999999"}) {
    auto r = ValveAddress{bad}.as_index();
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << bad;
  }
}

TEST(Capabilities, ValveStateNames) {
  EXPECT_EQ(to_string(ValveState::Open), "open");
  EXPECT_EQ(to_string(ValveState::Closed), "closed");
  EXPECT_EQ(to_string(ValveState::Unknown), "unknown");
}
