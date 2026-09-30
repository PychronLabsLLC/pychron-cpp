// ProXR relay driver over scripted, replayed and hooked SimTransports.

#include "pychron/devices/proxr_relay.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions opts(int retries = 0) {
  TransportOptions o;
  o.name = "valve_bus";
  o.timeout = 50ms;
  o.retries = retries;
  return o;
}

const Bytes kAck{0x55};

// Unknown for an error, so a failed read cannot pass as Open/Closed.
ValveState state(const Result<ValveState>& r) { return r ? *r : ValveState::Unknown; }

SimStep bank(std::uint8_t b) { return {Bytes{0xFE, 0x31, b}, kAck}; }

std::unique_ptr<SimTransport> open_scripted(std::vector<SimStep> steps) {
  auto t = SimTransport::scripted(std::move(steps), opts());
  EXPECT_TRUE(t->open());
  return t;
}


// Two drivers sharing one bus, switching relays in different banks at the
// same time. A per-driver lock cannot stop one driver's bank select landing
// between the other's select and relay command; a bus transaction can.
TEST(ProxrRelay, DriversSharingABusNeverSplitSelectAndSwitch) {
  ProxrBoardSim board;
  auto bus = SimTransport::hooked(board.hook());
  ASSERT_TRUE(bus->open());
  ProxrRelay first("first", *bus);
  ProxrRelay second("second", *bus);

  // Bank 1 holds addresses 0..7, bank 2 holds 8..15.
  auto flip = [](ProxrRelay& relay, int base) {
    for (int round = 0; round < 25; ++round) {
      for (int r = 0; r < 8; ++r) {
        const ValveAddress a{std::to_string(base + r)};
        ASSERT_TRUE(relay.open(a));
        ASSERT_EQ(*relay.read(a), ValveState::Open);
        ASSERT_TRUE(relay.close(a));
      }
    }
    for (int r = 0; r < 8; r += 2) ASSERT_TRUE(relay.open(ValveAddress{std::to_string(base + r)}));
  };
  std::thread a([&] { flip(first, 0); });
  std::thread b([&] { flip(second, 8); });
  a.join();
  b.join();

  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(board.energized(i), i % 2 == 0) << "relay " << i;
  }
  for (int i = 16; i < 256; ++i) ASSERT_FALSE(board.energized(i)) << "stray relay " << i;
}

}  // namespace

TEST(ProxrRelay, OpenSelectsBankThenEnergizesRelay) {
  auto t = open_scripted({bank(1), {Bytes{0xFE, 0x09}, kAck}});
  ProxrRelay relay("actuator1", *t);
  ASSERT_TRUE(relay.open(ValveAddress{"1"}));
  EXPECT_TRUE(t->verify());
  EXPECT_EQ(relay.health().state, DeviceState::Ok);
}

TEST(ProxrRelay, CloseDeEnergizesRelayInItsBank) {
  auto t = open_scripted({bank(2), {Bytes{0xFE, 0x03}, kAck}});
  ProxrRelay relay("actuator1", *t);
  ASSERT_TRUE(relay.close(ValveAddress{"11"}));
  EXPECT_TRUE(t->verify());
}

TEST(ProxrRelay, ReadMapsEnergizedToOpen) {
  auto t = open_scripted({bank(1), {Bytes{0xFE, 0x12}, Bytes{0x01}},  //
                          bank(1), {Bytes{0xFE, 0x12}, Bytes{0x00}}});
  ProxrRelay relay("actuator1", *t);
  EXPECT_EQ(state(relay.read(ValveAddress{"2"})), ValveState::Open);
  EXPECT_EQ(state(relay.read(ValveAddress{"2"})), ValveState::Closed);
  EXPECT_TRUE(t->verify());
}

TEST(ProxrRelay, HighestAddressUsesLastBank) {
  auto t = open_scripted({bank(32), {Bytes{0xFE, 0x0F}, kAck}});
  ProxrRelay relay("actuator1", *t);
  ASSERT_TRUE(relay.open(ValveAddress{"255"}));
  EXPECT_TRUE(t->verify());
}

TEST(ProxrRelay, BadAddressIsConfigAndSendsNothing) {
  auto t = open_scripted({});
  ProxrRelay relay("actuator1", *t);
  for (const char* a : {"abc", "256", "-1", ""}) {
    auto r = relay.open(ValveAddress{a});
    ASSERT_FALSE(r) << a;
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
    EXPECT_EQ(r.error().device, "actuator1");
  }
  EXPECT_TRUE(t->written().empty());
}

TEST(ProxrRelay, BankSelectNakIsProtocolAndSkipsRelayCommand) {
  auto t = open_scripted({{Bytes{0xFE, 0x31, 0x01}, Bytes{0x00}}});
  ProxrRelay relay("actuator1", *t);
  auto r = relay.open(ValveAddress{"1"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "actuator1");
  EXPECT_TRUE(t->verify());  // relay command never sent
  EXPECT_EQ(relay.health().state, DeviceState::Degraded);
}

TEST(ProxrRelay, RelayCommandNakIsProtocol) {
  auto t = open_scripted({bank(1), {Bytes{0xFE, 0x08}, to_bytes("X")}});
  ProxrRelay relay("actuator1", *t);
  auto r = relay.open(ValveAddress{"0"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("\"X\""), std::string::npos);
}

TEST(ProxrRelay, GarbledStatusIsProtocol) {
  auto t = open_scripted({bank(1), {Bytes{0xFE, 0x10}, Bytes{0x07}}});
  ProxrRelay relay("actuator1", *t);
  auto r = relay.read(ValveAddress{"0"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ProxrRelay, NoReplyIsTimeout) {
  auto t = open_scripted({bank(1)});
  t->drop_next();
  ProxrRelay relay("actuator1", *t);
  auto r = relay.open(ValveAddress{"1"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.error().device, "valve_bus");  // the transport attributes wire failures to itself
  EXPECT_EQ(relay.health().consecutive_failures, 1u);
}

TEST(ProxrRelay, ClosedTransportIsNotConnected) {
  auto t = SimTransport::scripted({}, opts());  // never opened
  ProxrRelay relay("actuator1", *t);
  auto r = relay.read(ValveAddress{"1"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
}

TEST(ProxrRelay, ReplaysTrace) {
  auto t = SimTransport::replay(PYCHRON_TRACES_DIR "/proxr/open_read_close_read.trace", opts());
  ASSERT_TRUE(t) << t.error().what;
  ASSERT_TRUE((*t)->open());
  ProxrRelay relay("actuator1", **t);
  ValveAddress a{"9"};
  ASSERT_TRUE(relay.open(a));
  EXPECT_EQ(state(relay.read(a)), ValveState::Open);
  ASSERT_TRUE(relay.close(a));
  EXPECT_EQ(state(relay.read(a)), ValveState::Closed);
  EXPECT_TRUE((*t)->verify());
}

TEST(ProxrRelayRegistry, RegisteredWithSchema) {
  const DriverRegistry& reg = DriverRegistry::global();
  ASSERT_TRUE(reg.contains("proxr_relay"));
  const DriverSchema* s = reg.schema("proxr_relay");
  ASSERT_NE(s, nullptr);
  EXPECT_FALSE(s->summary.empty());
  EXPECT_NE(describe(*s).find("proxr_relay"), std::string::npos);
}

TEST(ProxrRelayRegistry, CreatesValveActuator) {
  auto t = SimTransport::scripted({}, opts());
  toml::table options{{"kind", "proxr_relay"}, {"transport", "valve_bus"}};
  auto made = DriverRegistry::global().create("proxr_relay", *t, options, DriverContext{"actuator1", nullptr});
  ASSERT_TRUE(made) << made.error().what;
  EXPECT_EQ((*made)->name(), "actuator1");
  EXPECT_NE(capability<IValveActuator>(**made), nullptr);
}

TEST(ProxrRelayRegistry, RejectsUndeclaredKeys) {
  auto t = SimTransport::scripted({}, opts());
  toml::table options{{"kind", "proxr_relay"}, {"transport", "valve_bus"}, {"channels", 3}};
  auto made = DriverRegistry::global().create("proxr_relay", *t, options, DriverContext{"actuator1", nullptr});
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error().kind, ErrorKind::Config);
}

// --- driver against the SimSystem hook model -------------------------------

TEST(ProxrRelaySim, StatePersistsAcrossBanks) {
  ProxrBoardSim board;
  auto t = SimTransport::hooked(board.hook(), opts());
  ASSERT_TRUE(t->open());
  ProxrRelay relay("actuator1", *t);

  ASSERT_TRUE(relay.open(ValveAddress{"3"}));
  ASSERT_TRUE(relay.open(ValveAddress{"200"}));
  EXPECT_TRUE(board.energized(3));
  EXPECT_TRUE(board.energized(200));
  EXPECT_FALSE(board.energized(11));  // same relay index, other bank

  EXPECT_EQ(state(relay.read(ValveAddress{"3"})), ValveState::Open);
  EXPECT_EQ(state(relay.read(ValveAddress{"11"})), ValveState::Closed);
  ASSERT_TRUE(relay.close(ValveAddress{"200"}));
  EXPECT_EQ(state(relay.read(ValveAddress{"200"})), ValveState::Closed);
  EXPECT_EQ(state(relay.read(ValveAddress{"3"})), ValveState::Open);
}

TEST(ProxrRelaySim, ReadReportsHardwareNotLastCommand) {
  ProxrBoardSim board;
  auto t = SimTransport::hooked(board.hook(), opts());
  ASSERT_TRUE(t->open());
  ProxrRelay relay("actuator1", *t);
  ASSERT_TRUE(relay.open(ValveAddress{"5"}));
  board.set_energized(5, false);  // e.g. someone flipped it at the panel
  EXPECT_EQ(state(relay.read(ValveAddress{"5"})), ValveState::Closed);
}

TEST(ProxrRelaySim, TransportRetryRecoversDroppedReply) {
  ProxrBoardSim board;
  auto t = SimTransport::hooked(board.hook(), opts(/*retries=*/1));
  ASSERT_TRUE(t->open());
  ProxrRelay relay("actuator1", *t);
  t->drop_next();
  ASSERT_TRUE(relay.open(ValveAddress{"4"}));
  EXPECT_TRUE(board.energized(4));
}

TEST(ProxrRelaySim, ListenerSeesEveryActuation) {
  std::vector<std::pair<std::int64_t, bool>> events;
  ProxrBoardSim board([&](std::int64_t index, bool on) { events.emplace_back(index, on); });
  auto t = SimTransport::hooked(board.hook(), opts());
  ASSERT_TRUE(t->open());
  ProxrRelay relay("actuator1", *t);
  ASSERT_TRUE(relay.open(ValveAddress{"17"}));
  ASSERT_TRUE(relay.read(ValveAddress{"17"}));
  ASSERT_TRUE(relay.close(ValveAddress{"17"}));
  EXPECT_EQ(events, (std::vector<std::pair<std::int64_t, bool>>{{17, true}, {17, false}}));
}

TEST(ProxrRelaySim, ConcurrentCallersNeverCrossBanks) {
  std::map<std::int64_t, std::vector<bool>> events;  // written on the transport worker only
  ProxrBoardSim board([&](std::int64_t index, bool on) { events[index].push_back(on); });
  auto t = SimTransport::hooked(board.hook(), opts());
  ASSERT_TRUE(t->open());
  ProxrRelay relay("actuator1", *t);

  // Same relay index (1) in two banks: an interleaved bank select would
  // actuate the wrong valve.
  auto worker = [&](const char* address, int n) {
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(relay.open(ValveAddress{address}));
      ASSERT_TRUE(relay.close(ValveAddress{address}));
    }
    ASSERT_TRUE(relay.open(ValveAddress{address}));
  };
  std::thread a(worker, "1", 50);
  std::thread b(worker, "9", 50);
  a.join();
  b.join();
  EXPECT_TRUE(board.energized(1));
  EXPECT_TRUE(board.energized(9));

  // Each valve saw exactly its own on/off/.../on sequence.
  std::vector<bool> expected;
  for (int i = 0; i < 50; ++i) {
    expected.push_back(true);
    expected.push_back(false);
  }
  expected.push_back(true);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[1], expected);
  EXPECT_EQ(events[9], expected);
}
