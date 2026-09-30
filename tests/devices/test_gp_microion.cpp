#include "pychron/devices/gp_microion.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions bus_options() {
  TransportOptions o;
  o.name = "microion_bus";
  o.timeout = 50ms;
  return o;
}

std::unique_ptr<SimTransport> opened(std::unique_ptr<SimTransport> sim) {
  EXPECT_TRUE(sim->open());
  return sim;
}

std::unique_ptr<SimTransport> open_scripted(std::vector<SimStep> steps, TransportOptions o = bus_options()) {
  return opened(SimTransport::scripted(std::move(steps), std::move(o)));
}

std::unique_ptr<SimTransport> open_hooked(SimTransport::Hook hook) {
  return opened(SimTransport::hooked(std::move(hook), bus_options()));
}

SimStep step(std::string_view tx, std::string_view rx) { return SimStep{to_bytes(tx), to_bytes(rx), {}}; }

toml::table table_of(std::string_view text) {
  auto r = toml::parse(text, std::string_view("extraction_line.toml"));
  EXPECT_TRUE(r) << r.error().description();
  return r ? std::move(r).table() : toml::table{};
}

void expect_verified(const SimTransport& sim) {
  auto v = sim.verify();
  EXPECT_TRUE(v) << (v ? "" : to_string(v.error()));
}

}  // namespace

// --- scripted transport -----------------------------------------------------

TEST(GpMicroIon, ReadsIonGaugeByDefault) {
  auto sim = open_scripted({step("#01DS IG\r", "*01 1.20E-06\r")});
  GpMicroIon g("bone_ig", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_TRUE(p) << to_string(p.error());
  EXPECT_DOUBLE_EQ(*p, 1.2e-6);
  EXPECT_EQ(g.health().state, DeviceState::Ok);
  expect_verified(*sim);
}

TEST(GpMicroIon, ReadsEachChannelAtItsAddress) {
  auto sim = open_scripted({step("#0ADS CG1\r", "*0A 7.60E+02\r"), step("#0ADS CG2\r", "*0A 1.00E-03\r")});
  GpMicroIon g("mi", *sim, 0x0A, {2, 3});
  EXPECT_EQ(g.pressure_channels(), (std::vector<int>{2, 3}));

  EXPECT_DOUBLE_EQ(*g.read_pressure(), 760.0);
  EXPECT_DOUBLE_EQ(*g.read_pressure(3), 1e-3);
  expect_verified(*sim);
}

TEST(GpMicroIon, UnconfiguredChannelIsConfigErrorWithoutTraffic) {
  auto sim = open_scripted({});
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure(2);
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
  EXPECT_EQ(p.error().device, "mi");
  EXPECT_TRUE(sim->written().empty());
}

TEST(GpMicroIon, GaugeOffIsProtocolError) {
  auto sim = open_scripted({step("#01DS IG\r", "*01 9.90E+09\r")});
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(p.error().device, "mi");
  EXPECT_EQ(g.health().state, DeviceState::Degraded);
}

TEST(GpMicroIon, ControllerErrorIsProtocolError) {
  auto sim = open_scripted({step("#01DS IG\r", "?01 SYNTX ER\r")});
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Protocol);
  EXPECT_NE(p.error().what.find("SYNTX ER"), std::string::npos) << p.error().what;
}

TEST(GpMicroIon, DroppedReplyIsTimeoutNamingTransport) {
  auto sim = open_scripted({step("#01DS IG\r", "*01 1.20E-06\r")});
  sim->drop_next();
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(p.error().device, "microion_bus");
  EXPECT_EQ(g.health().last_error, ErrorKind::Timeout);
}

TEST(GpMicroIon, GarbledReplyFailsRead) {
  auto sim = open_scripted({step("#01DS IG\r", "*01 1.20E-06\r")});
  sim->garble_next();
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_TRUE(p.error().kind == ErrorKind::Protocol || p.error().kind == ErrorKind::Timeout)
      << to_string(p.error());
}

TEST(GpMicroIon, RetriesAfterTimeoutWhenTransportAllows) {
  auto o = bus_options();
  o.retries = 1;
  auto sim = open_scripted({step("#01DS IG\r", "*01 1.20E-06\r"), step("#01DS IG\r", "*01 2.00E-07\r")}, o);
  sim->drop_next();
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_TRUE(p) << to_string(p.error());
  EXPECT_DOUBLE_EQ(*p, 2e-7);
  expect_verified(*sim);
}

TEST(GpMicroIon, ClosedTransportIsNotConnected) {
  auto sim = SimTransport::scripted({step("#01DS IG\r", "*01 1.20E-06\r")}, bus_options());
  GpMicroIon g("mi", *sim, 1, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::NotConnected);
}

TEST(GpMicroIon, SwitchesIonGauge) {
  auto sim = open_scripted({step("#01IG1 ON\r", "*01 PROGM OK\r"), step("#01IG1 OFF\r", "?01 INVALID\r")});
  GpMicroIon g("mi", *sim, 1, {1});

  EXPECT_TRUE(g.set_ion_gauge(true));
  auto off = g.set_ion_gauge(false);
  ASSERT_FALSE(off);
  EXPECT_EQ(off.error().kind, ErrorKind::Protocol);
  expect_verified(*sim);
}

TEST(GpMicroIon, SampleIsStampedWithInjectedClock) {
  auto sim = open_scripted({step("#01DS IG\r", "*01 3.00E-06\r")});
  ManualClock clock(TimePoint{} + 7s);
  GpMicroIon g("mi", *sim, 1, {1}, &clock);

  auto s = g.sample();
  ASSERT_TRUE(s) << to_string(s.error());
  EXPECT_EQ(s->device, "mi");
  EXPECT_EQ(s->ts, TimePoint{} + 7s);
  EXPECT_DOUBLE_EQ(s->value, 3e-6);
}

TEST(GpMicroIon, ImplementsCapabilities) {
  auto sim = open_scripted({});
  GpMicroIon g("mi", *sim, 1, {1});
  Device& d = g;
  EXPECT_NE(capability<IPressureGauge>(d), nullptr);
  EXPECT_NE(capability<IScannable>(d), nullptr);
  EXPECT_NE(capability<IChannelPressureGauge>(d), nullptr);
}

// --- registry -----------------------------------------------------------------

TEST(GpMicroIonRegistry, RegisteredWithSchema) {
  const auto* schema = DriverRegistry::global().schema("gp_microion");
  ASSERT_NE(schema, nullptr);
  ASSERT_EQ(schema->keys.size(), 2u);
  EXPECT_EQ(schema->keys[0].name, "address");
  EXPECT_EQ(schema->keys[0].type, KeyType::Integer);
  EXPECT_FALSE(schema->keys[0].required);
  EXPECT_EQ(schema->keys[1].name, "channels");
  EXPECT_EQ(schema->keys[1].type, KeyType::IntegerArray);
}

TEST(GpMicroIonRegistry, CreatesFromConfigTable) {
  auto sim = open_scripted({step("#05DS CG1\r", "*05 4.00E-02\r")});
  auto t = table_of("kind = 'gp_microion'\ntransport = 'microion_bus'\naddress = 5\nchannels = [2, 1]\n");
  auto d = DriverRegistry::global().create("gp_microion", *sim, t, {"bone_gauge", nullptr});
  ASSERT_TRUE(d) << to_string(d.error());
  EXPECT_EQ((*d)->name(), "bone_gauge");
  EXPECT_EQ(capability<IChannelPressureGauge>(**d)->pressure_channels(), (std::vector<int>{2, 1}));
  EXPECT_DOUBLE_EQ(*capability<IPressureGauge>(**d)->read_pressure(), 4e-2);
  expect_verified(*sim);
}

TEST(GpMicroIonRegistry, DefaultsToAddressOneIonGauge) {
  auto sim = open_scripted({step("#01DS IG\r", "*01 1.00E-09\r")});
  auto t = table_of("kind = 'gp_microion'\ntransport = 'microion_bus'\n");
  auto d = DriverRegistry::global().create("gp_microion", *sim, t, {"mi", nullptr});
  ASSERT_TRUE(d) << to_string(d.error());
  EXPECT_EQ(capability<IChannelPressureGauge>(**d)->pressure_channels(), (std::vector<int>{1}));
  EXPECT_DOUBLE_EQ(*capability<IPressureGauge>(**d)->read_pressure(), 1e-9);
}

TEST(GpMicroIonRegistry, RejectsBadOptions) {
  auto sim = open_scripted({});
  for (const char* extra :
       {"channels = []", "channels = [0]", "channels = [4]", "channels = [1, 1]", "channels = ['IG']",
        "address = -1", "address = 256", "address = '01'"}) {
    auto t = table_of(std::string("kind = 'gp_microion'\ntransport = 'microion_bus'\n") + extra + "\n");
    auto d = DriverRegistry::global().create("gp_microion", *sim, t, {"mi", nullptr});
    ASSERT_FALSE(d) << extra;
    EXPECT_EQ(d.error().kind, ErrorKind::Config) << extra;
    EXPECT_EQ(d.error().device, "mi") << extra;
  }
}

// --- replay -----------------------------------------------------------------------

TEST(GpMicroIonReplay, ReplaysCommittedTrace) {
  auto sim = SimTransport::replay(std::string(PYCHRON_TRACE_DIR) + "/granville_phillips/microion_read_sensors.trace",
                                  bus_options());
  ASSERT_TRUE(sim) << to_string(sim.error());
  ASSERT_TRUE((*sim)->open());
  GpMicroIon g("mi", **sim, 1, {1, 2, 3});

  EXPECT_TRUE(g.set_ion_gauge(true));
  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 2.3e-9);
  EXPECT_DOUBLE_EQ(*g.read_pressure(2), 1.1e-3);
  auto unplugged = g.read_pressure(3);
  ASSERT_FALSE(unplugged);
  EXPECT_EQ(unplugged.error().kind, ErrorKind::Protocol);
  expect_verified(**sim);
}

// --- SimSystem hook contract ---------------------------------------------------------

TEST(GpMicroIonSimHook, ServesModelPressures) {
  std::atomic<double> chamber{1e-6};
  MicroIonSimModel model;
  model.address = 2;
  model.pressure = [&](int channel) -> std::optional<double> {
    if (channel == 1) return chamber.load();
    if (channel == 2) return 5e-3;
    return std::nullopt;
  };
  auto sim = open_hooked(microion_sim_hook(model));
  GpMicroIon g("mi", *sim, 2, {1, 2, 3});

  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 1e-6);
  chamber = 4e-8;  // SimSystem state moves; the next read follows it
  EXPECT_NEAR(*g.read_pressure(1), 4e-8, 1e-12);
  EXPECT_DOUBLE_EQ(*g.read_pressure(2), 5e-3);
  auto none = g.read_pressure(3);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Protocol);
}

TEST(GpMicroIonSimHook, IonGaugeFilamentState) {
  MicroIonSimModel model;
  model.pressure = [](int) -> std::optional<double> { return 1e-8; };
  model.ion_gauge_on = false;
  auto sim = open_hooked(microion_sim_hook(model));
  GpMicroIon g("mi", *sim, 1, {1, 2});

  EXPECT_FALSE(g.read_pressure(1));              // filament off reads 9.90E+09
  EXPECT_DOUBLE_EQ(*g.read_pressure(2), 1e-8);   // convectrons are unaffected
  ASSERT_TRUE(g.set_ion_gauge(true));
  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 1e-8);
  ASSERT_TRUE(g.set_ion_gauge(false));
  EXPECT_FALSE(g.read_pressure(1));
}

TEST(GpMicroIonSimHook, AnswersOnlyItsAddressAndRejectsSyntax) {
  MicroIonSimModel model;
  model.address = 1;
  model.pressure = [](int) -> std::optional<double> { return 1.0; };
  auto hook = microion_sim_hook(model);

  EXPECT_EQ(hook(to_bytes("#01DS IG\r")), to_bytes("*01 1.00E+00\r"));
  EXPECT_EQ(hook(to_bytes("#02DS IG\r")), Bytes{});  // another device on the bus
  EXPECT_EQ(hook(to_bytes("#01XX\r")), to_bytes("?01 SYNTX ER\r"));
  EXPECT_EQ(hook(to_bytes("garbage")), Bytes{});
}

// --- vendor blindness --------------------------------------------------------------

// What a manager does: read through the capability, knowing only the kind
// string from config. Two vendors, two wire protocols, one code path.
TEST(PressureGaugeCapability, IsVendorBlind) {
  auto maxigauge_wire = open_hooked(maxigauge_sim_hook(
      {[](int) -> std::optional<double> { return 3e-9; }, codec::maxigauge::Units::Torr}));
  MicroIonSimModel mi_model;
  mi_model.pressure = [](int) -> std::optional<double> { return 3e-9; };
  auto microion_wire = open_hooked(microion_sim_hook(mi_model));

  struct Case {
    const char* kind;
    SimTransport* wire;
  };
  for (Case c : {Case{"pfeiffer_maxigauge", maxigauge_wire.get()}, Case{"gp_microion", microion_wire.get()}}) {
    auto t = table_of(std::string("kind = '") + c.kind + "'\ntransport = 'bus'\n");
    auto d = DriverRegistry::global().create(c.kind, *c.wire, t, {"gauge", nullptr});
    ASSERT_TRUE(d) << c.kind << ": " << to_string(d.error());

    IPressureGauge* gauge = capability<IPressureGauge>(**d);
    ASSERT_NE(gauge, nullptr) << c.kind;
    auto p = gauge->read_pressure();
    ASSERT_TRUE(p) << c.kind << ": " << to_string(p.error());
    EXPECT_DOUBLE_EQ(*p, 3e-9) << c.kind;

    IScannable* scan = capability<IScannable>(**d);
    ASSERT_NE(scan, nullptr) << c.kind;
    auto s = scan->sample();
    ASSERT_TRUE(s) << c.kind;
    EXPECT_EQ(s->device, "gauge");
  }
}

TEST(GpMicroIonSimHook, ConcurrentReadsNeverCrossChannels) {
  MicroIonSimModel model;
  model.pressure = [](int channel) -> std::optional<double> { return channel * 1e-8; };
  auto sim = open_hooked(microion_sim_hook(model));
  GpMicroIon g("mi", *sim, 1, {1, 2});

  std::atomic<int> wrong{0};
  auto reader = [&](int channel) {
    for (int i = 0; i < 50; ++i) {
      auto p = g.read_pressure(channel);
      if (!p || *p != channel * 1e-8) ++wrong;
    }
  };
  std::thread a(reader, 1), b(reader, 2);
  a.join();
  b.join();
  EXPECT_EQ(wrong.load(), 0);
}
