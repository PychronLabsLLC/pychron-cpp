// qtegra_valves: valves through Qtegra, alone or on the spectrometer's
// connection (plan 2026-10-05, task A2).
#include "pychron/devices/spectrometer/qtegra_valves.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/transport/link_transport.hpp"
#include "spectrometer/legacy/sim_util.hpp"
#include "valve_conformance.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

TransportOptions fast(const Clock* clock = nullptr) {
  TransportOptions o;
  o.name = "qtegra";
  o.timeout = 50ms;
  o.clock = clock;
  return o;
}

Result<std::unique_ptr<Device>> make(Transport& t, std::string_view toml, std::string name,
                                     const Clock* clock = nullptr) {
  return DriverRegistry::global().create("qtegra_valves", t, legacy_test::table_of(toml),
                                         DriverContext{std::move(name), clock});
}

// The valves owning a Qtegra connection of their own, on `clock` (real time
// when null).
struct Rig final : pychron::test::ValveRig {
  explicit Rig(const std::string& link, const Clock* on = nullptr) : clock(on) {
    EXPECT_TRUE(bus->open());
    auto made = make(*bus, "link = \"" + link + "\"", "switch_controller", clock);
    EXPECT_TRUE(made) << made.error().what;
    if (made) device = std::move(*made);
  }

  const Clock* clock;
  std::shared_ptr<QtegraSimModel> model = std::make_shared<QtegraSimModel>();
  pychron::test::WireTap wire{qtegra_sim_hook(model), to_bytes("ERROR: no such valve\r\n")};
  std::unique_ptr<SimTransport> bus = SimTransport::hooked(wire.hook(), fast(clock));
  std::unique_ptr<Device> device;

  IValveActuator& actuator() override { return *capability<IValveActuator>(*device); }
  ValveAddress first() override { return {"Valve 1_9 Set"}; }
  ValveAddress second() override { return {"Pipet Ref. Out Set"}; }
  std::optional<ValveAddress> bad() override { return ValveAddress{"A,B"}; }
  pychron::test::WireTap* tap() override { return &wire; }
};

}  // namespace

// On a virtual clock: every Qtegra command is Safe, so the link tries a
// silent instrument for its call window, which then costs no real time.
struct QtegraValvesVirtual : pychron::testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};
};

TEST_F(QtegraValvesVirtual, PassTheValveConformanceSuite) {
  Rig rig("qv_conformance", &clock);
  pychron::test::expect_valve_conformance(rig);
}

TEST(QtegraValves, SpeakLegacyQtegraValveCommands) {
  Rig rig("qv_wire");
  auto& v = rig.actuator();
  ASSERT_TRUE(v.open(ValveAddress{"Valve 1_9 Set"}));
  ASSERT_TRUE(v.read(ValveAddress{"Valve 1_9 Set"}));
  ASSERT_TRUE(v.close(ValveAddress{"Valve 1_9 Set"}));
  std::lock_guard lock(rig.model->mutex);
  EXPECT_EQ(rig.model->commands, (std::vector<std::string>{"Open Valve 1_9 Set", "GetValveState Valve 1_9 Set",
                                                            "Close Valve 1_9 Set"}));
  EXPECT_FALSE(rig.model->valves.at("Valve 1_9 Set"));
}

TEST(QtegraValves, AnErrorReplyIsNeverAState) {
  Rig rig("qv_error");
  rig.wire.set(pychron::test::WireTap::Mode::Garbage);  // every reply: ERROR: no such valve
  auto opened = rig.actuator().open(ValveAddress{"Nope"});
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(opened.error().device, "switch_controller");
  auto state = rig.actuator().read(ValveAddress{"Nope"});
  ASSERT_FALSE(state);
  EXPECT_EQ(state.error().kind, ErrorKind::Protocol);
}

TEST(QtegraValves, ShareTheSpectrometersConnection) {
  auto model = std::make_shared<QtegraSimModel>();
  model->dac = 4.5;
  auto sim = legacy_test::open_hooked(qtegra_sim_hook(model));
  auto spectrometer = DriverRegistry::global().create("thermo_qtegra", *sim, legacy_test::table_of("link = \"melb\""),
                                                      DriverContext{"argus"});
  ASSERT_TRUE(spectrometer) << spectrometer.error().what;

  LinkTransport borrowed("switch_controller", "melb");
  auto made = make(borrowed, "", "switch_controller");
  ASSERT_TRUE(made) << made.error().what;
  auto* valves = dynamic_cast<QtegraValves*>(made->get());
  ASSERT_NE(valves, nullptr);
  EXPECT_FALSE(valves->owns_link());
  ASSERT_TRUE(valves->open(ValveAddress{"Valve 1_1 Set"}));
  EXPECT_EQ(*valves->read(ValveAddress{"Valve 1_1 Set"}), ValveState::Open);
  std::lock_guard lock(model->mutex);
  EXPECT_TRUE(model->valves.at("Valve 1_1 Set"));  // the spectrometer's Qtegra did it
}

TEST(QtegraValves, AMissingOwnerIsNotConnected) {
  LinkTransport borrowed("switch_controller", "nobody_owns_this");
  auto made = make(borrowed, "", "switch_controller");
  ASSERT_TRUE(made) << made.error().what;
  auto r = capability<IValveActuator>(**made)->open(ValveAddress{"A"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
}

TEST(QtegraValves, RejectAnUnknownTerminator) {
  Rig rig("qv_term");
  auto r = make(*rig.bus, "terminator = \"nul\"\nlink = \"qv_term2\"", "x");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}
