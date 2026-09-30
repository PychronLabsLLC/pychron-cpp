#include "pychron/devices/pfeiffer_maxigauge.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace std::chrono_literals;
namespace mg = pychron::codec::maxigauge;

namespace {

const Bytes kAck = to_bytes("\x06\r\n");
const Bytes kNak = to_bytes("\x15\r\n");
const Bytes kEnq = Bytes{0x05};

TransportOptions bus_options() {
  TransportOptions o;
  o.name = "gauge_net";
  o.timeout = 50ms;
  return o;
}

std::unique_ptr<SimTransport> opened(std::unique_ptr<SimTransport> sim) {
  EXPECT_TRUE(sim->open());
  return sim;
}

std::unique_ptr<SimTransport> open_scripted(std::vector<SimStep> steps, TransportOptions o) {
  return opened(SimTransport::scripted(std::move(steps), std::move(o)));
}

std::unique_ptr<SimTransport> open_hooked(SimTransport::Hook hook, TransportOptions o) {
  return opened(SimTransport::hooked(std::move(hook), std::move(o)));
}

// Mnemonic, ACK, ENQ, data: one complete query on the wire.
std::vector<SimStep> query(std::string_view mnemonic, std::string_view data) {
  return {SimStep{to_bytes(mnemonic), kAck, {}}, SimStep{kEnq, to_bytes(data), {}}};
}

std::vector<SimStep> concat(std::initializer_list<std::vector<SimStep>> parts) {
  std::vector<SimStep> out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

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

TEST(PfeifferMaxiGauge, ReadsFirstConfiguredChannelByDefault) {
  auto sim = open_scripted(query("PR2\r\n", "0,+1.2300E-08\r\n"), bus_options());
  PfeifferMaxiGauge g("ig_controller", *sim, {2, 3});

  auto p = g.read_pressure();
  ASSERT_TRUE(p) << to_string(p.error());
  EXPECT_DOUBLE_EQ(*p, 1.23e-8);
  EXPECT_EQ(g.health().state, DeviceState::Ok);
  expect_verified(*sim);
}

TEST(PfeifferMaxiGauge, ReadsEachChannel) {
  auto sim = open_scripted(
      concat({query("PR1\r\n", "0,+1.0000E-08\r\n"), query("PR3\r\n", "1,+5.0000E-11\r\n")}), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1, 3});
  EXPECT_EQ(g.pressure_channels(), (std::vector<int>{1, 3}));

  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 1e-8);
  EXPECT_DOUBLE_EQ(*g.read_pressure(3), 5e-11);  // underrange still reports the limit
  expect_verified(*sim);
}

TEST(PfeifferMaxiGauge, UnconfiguredChannelIsConfigErrorWithoutTraffic) {
  auto sim = open_scripted({}, bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure(4);
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
  EXPECT_EQ(p.error().device, "ig");
  EXPECT_TRUE(sim->written().empty());
}

TEST(PfeifferMaxiGauge, NakIsProtocolErrorAndSkipsEnquiry) {
  auto sim = open_scripted({SimStep{to_bytes("PR1\r\n"), kNak, {}}}, bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(p.error().device, "ig");
  EXPECT_EQ(g.health().state, DeviceState::Degraded);
  EXPECT_EQ(sim->written().size(), 1u);
  expect_verified(*sim);
}

TEST(PfeifferMaxiGauge, SensorFaultIsProtocolError) {
  auto sim = open_scripted(query("PR1\r\n", "4,+0.0000E+00\r\n"), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Protocol);
  EXPECT_NE(p.error().what.find("sensor off"), std::string::npos) << p.error().what;
}

TEST(PfeifferMaxiGauge, DroppedReplyIsTimeout) {
  auto sim = open_scripted(query("PR1\r\n", "0,+1.0000E-08\r\n"), bus_options());
  sim->drop_next();
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(p.error().device, "gauge_net");  // wire errors name the transport
  EXPECT_EQ(g.health().last_error, ErrorKind::Timeout);
}

TEST(PfeifferMaxiGauge, MalformedDataIsProtocolError) {
  auto sim = open_scripted(query("PR1\r\n", "0,+1.0E-0x\r\n"), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(g.health().last_error, ErrorKind::Protocol);
}

TEST(PfeifferMaxiGauge, GarbledAckFailsRead) {
  auto sim = open_scripted(query("PR1\r\n", "0,+1.0000E-08\r\n"), bus_options());
  sim->garble_next();
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_TRUE(p.error().kind == ErrorKind::Protocol || p.error().kind == ErrorKind::Timeout)
      << to_string(p.error());
}

TEST(PfeifferMaxiGauge, RetriesAfterTimeoutWhenTransportAllows) {
  auto o = bus_options();
  o.retries = 1;
  auto sim = open_scripted(
      concat({{SimStep{to_bytes("PR1\r\n"), kAck, {}}, SimStep{to_bytes("PR1\r\n"), kAck, {}}},
              {SimStep{kEnq, to_bytes("0,+2.0000E-07\r\n"), {}}}}),
      o);
  sim->drop_next();
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_TRUE(p) << to_string(p.error());
  EXPECT_DOUBLE_EQ(*p, 2e-7);
  expect_verified(*sim);
}

TEST(PfeifferMaxiGauge, ClosedTransportIsNotConnected) {
  auto sim = SimTransport::scripted(query("PR1\r\n", "0,+1.0000E-08\r\n"), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});

  auto p = g.read_pressure();
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::NotConnected);
  EXPECT_TRUE(sim->written().empty());
}

TEST(PfeifferMaxiGauge, SampleIsStampedWithInjectedClock) {
  auto sim = open_scripted(query("PR1\r\n", "0,+3.0000E-06\r\n"), bus_options());
  ManualClock clock(TimePoint{} + 42s);
  PfeifferMaxiGauge g("ig", *sim, {1}, &clock);

  auto s = g.sample();
  ASSERT_TRUE(s) << to_string(s.error());
  EXPECT_EQ(s->device, "ig");
  EXPECT_EQ(s->ts, TimePoint{} + 42s);
  EXPECT_DOUBLE_EQ(s->value, 3e-6);
}

TEST(PfeifferMaxiGauge, ReadAllChannels) {
  auto sim = open_scripted(
      query("PRX\r\n",
            "0,+1.0000E-08,0,+2.0000E-07,5,+0.0000E+00,5,+0.0000E+00,5,+0.0000E+00,5,+0.0000E+00\r\n"),
      bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1, 2});

  auto all = g.read_all();
  ASSERT_TRUE(all) << to_string(all.error());
  ASSERT_EQ(all->size(), 6u);
  EXPECT_EQ((*all)[1], (mg::Reading{mg::Status::Ok, 2e-7}));
  EXPECT_EQ((*all)[2].status, mg::Status::NoSensor);
  expect_verified(*sim);
}

TEST(PfeifferMaxiGauge, ReadUnits) {
  auto sim = open_scripted(query("UNI\r\n", "1\r\n"), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});
  EXPECT_EQ(*g.read_units(), mg::Units::Torr);
  expect_verified(*sim);
}

TEST(PfeifferMaxiGauge, ImplementsCapabilities) {
  auto sim = open_scripted({}, bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1});
  Device& d = g;
  EXPECT_NE(capability<IPressureGauge>(d), nullptr);
  EXPECT_NE(capability<IScannable>(d), nullptr);
  EXPECT_NE(capability<IChannelPressureGauge>(d), nullptr);
}

// --- registry -----------------------------------------------------------------

TEST(PfeifferMaxiGaugeRegistry, RegisteredWithSchema) {
  const auto* schema = DriverRegistry::global().schema("pfeiffer_maxigauge");
  ASSERT_NE(schema, nullptr);
  ASSERT_EQ(schema->keys.size(), 1u);
  EXPECT_EQ(schema->keys[0].name, "channels");
  EXPECT_EQ(schema->keys[0].type, KeyType::IntegerArray);
}

TEST(PfeifferMaxiGaugeRegistry, CreatesFromConfigTable) {
  auto sim = open_scripted(query("PR3\r\n", "0,+7.0000E-09\r\n"), bus_options());
  auto t = table_of("kind = 'pfeiffer_maxigauge'\ntransport = 'gauge_net'\nchannels = [3, 1, 2]\n");
  auto d = DriverRegistry::global().create("pfeiffer_maxigauge", *sim, t, {"ig_controller", nullptr});
  ASSERT_TRUE(d) << to_string(d.error());
  EXPECT_EQ((*d)->name(), "ig_controller");

  auto* multi = capability<IChannelPressureGauge>(**d);
  ASSERT_NE(multi, nullptr);
  EXPECT_EQ(multi->pressure_channels(), (std::vector<int>{3, 1, 2}));
  EXPECT_DOUBLE_EQ(*capability<IPressureGauge>(**d)->read_pressure(), 7e-9);
}

TEST(PfeifferMaxiGaugeRegistry, ChannelsDefaultToOne) {
  auto sim = open_scripted({}, bus_options());
  auto t = table_of("kind = 'pfeiffer_maxigauge'\ntransport = 'gauge_net'\n");
  auto d = DriverRegistry::global().create("pfeiffer_maxigauge", *sim, t, {"ig", nullptr});
  ASSERT_TRUE(d) << to_string(d.error());
  EXPECT_EQ(capability<IChannelPressureGauge>(**d)->pressure_channels(), (std::vector<int>{1}));
}

TEST(PfeifferMaxiGaugeRegistry, RejectsBadChannels) {
  auto sim = open_scripted({}, bus_options());
  for (const char* channels : {"[]", "[0]", "[7]", "[1, 1]", "['a']", "3"}) {
    auto t = table_of(std::string("kind = 'pfeiffer_maxigauge'\ntransport = 'gauge_net'\nchannels = ") +
                      channels + "\n");
    auto d = DriverRegistry::global().create("pfeiffer_maxigauge", *sim, t, {"ig", nullptr});
    ASSERT_FALSE(d) << channels;
    EXPECT_EQ(d.error().kind, ErrorKind::Config) << channels;
    EXPECT_EQ(d.error().device, "ig") << channels;
  }
}

// --- replay -----------------------------------------------------------------------

TEST(PfeifferMaxiGaugeReplay, ReplaysCommittedTrace) {
  auto sim = SimTransport::replay(std::string(PYCHRON_TRACE_DIR) + "/pfeiffer/maxigauge_read_channels.trace",
                                  bus_options());
  ASSERT_TRUE(sim) << to_string(sim.error());
  ASSERT_TRUE((*sim)->open());
  PfeifferMaxiGauge g("ig", **sim, {1, 2, 3});

  EXPECT_EQ(*g.read_units(), mg::Units::Torr);
  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 1.23e-8);
  EXPECT_DOUBLE_EQ(*g.read_pressure(2), 4.56e-7);
  auto missing = g.read_pressure(3);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Protocol);
  expect_verified(**sim);
}

// --- SimSystem hook contract ---------------------------------------------------------

TEST(PfeifferMaxiGaugeSimHook, ServesModelPressures) {
  std::atomic<double> chamber{1e-6};
  MaxiGaugeSimModel model;
  model.units = mg::Units::Mbar;
  model.pressure = [&](int channel) -> std::optional<double> {
    if (channel == 1) return chamber.load();
    if (channel == 2) return 2e-9;
    return std::nullopt;
  };
  auto sim = open_hooked(maxigauge_sim_hook(model), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1, 2, 3});

  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 1e-6);
  chamber = 5e-8;  // SimSystem state moves; the next read follows it
  EXPECT_DOUBLE_EQ(*g.read_pressure(1), 5e-8);
  EXPECT_DOUBLE_EQ(*g.read_pressure(2), 2e-9);
  auto none = g.read_pressure(3);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Protocol);

  EXPECT_EQ(*g.read_units(), mg::Units::Mbar);
  auto all = g.read_all();
  ASSERT_TRUE(all);
  EXPECT_EQ((*all)[1], (mg::Reading{mg::Status::Ok, 2e-9}));
  EXPECT_EQ((*all)[5].status, mg::Status::NoSensor);
}

TEST(PfeifferMaxiGaugeSimHook, NaksWhatTheGaugeWould) {
  MaxiGaugeSimModel model;
  model.pressure = [](int) -> std::optional<double> { return 1.0; };
  auto hook = maxigauge_sim_hook(model);

  EXPECT_EQ(hook(Bytes{0x05}), kNak);             // ENQ with nothing acknowledged
  EXPECT_EQ(hook(to_bytes("PR9\r\n")), kNak);     // no such channel
  EXPECT_EQ(hook(to_bytes("PR1\r\n")), kAck);
  EXPECT_EQ(hook(Bytes{0x05}), to_bytes("0,+1.0000E+00\r\n"));
  EXPECT_EQ(hook(Bytes{0x05}), to_bytes("0,+1.0000E+00\r\n"));  // ENQ repeats the last query
}

TEST(PfeifferMaxiGaugeSimHook, ConcurrentReadsNeverCrossChannels) {
  MaxiGaugeSimModel model;
  model.pressure = [](int channel) -> std::optional<double> { return channel * 1e-8; };
  auto sim = open_hooked(maxigauge_sim_hook(model), bus_options());
  PfeifferMaxiGauge g("ig", *sim, {1, 2});

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
