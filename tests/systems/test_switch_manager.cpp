// SwitchManager: lock, owner, interlocks, settle, read-back and events,
// against an in-memory actuator, plus one run over the real ProXR driver.

#include "pychron/systems/switch_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pychron/core/config/loader.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/devices/proxr_relay.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace pychron::systems;
using namespace std::chrono_literals;

namespace {

// Hardware double: remembers each address's state, logs every call and can
// be told to fail or to stick.
class FakeActuator final : public IValveActuator {
 public:
  explicit FakeActuator(const Clock* clock = nullptr) : clock_(clock) {}

  Result<void> open(const ValveAddress& a) override { return command(a, ValveState::Open); }
  Result<void> close(const ValveAddress& a) override { return command(a, ValveState::Closed); }

  Result<ValveState> read(const ValveAddress& a) override {
    std::lock_guard lk(m_);
    calls_.push_back("read " + a.value);
    if (clock_) read_times_.push_back(clock_->now());
    if (fail_read_) return fail(*fail_read_, "read failed", "fake");
    auto it = hw_.find(a.value);
    return it == hw_.end() ? ValveState::Closed : it->second;
  }

  void set_hw(const std::string& a, ValveState s) {
    std::lock_guard lk(m_);
    hw_[a] = s;
  }
  ValveState hw(const std::string& a) {
    std::lock_guard lk(m_);
    auto it = hw_.find(a);
    return it == hw_.end() ? ValveState::Closed : it->second;
  }
  void fail_commands(std::optional<ErrorKind> k) {
    std::lock_guard lk(m_);
    fail_command_ = k;
  }
  void fail_reads(std::optional<ErrorKind> k) {
    std::lock_guard lk(m_);
    fail_read_ = k;
  }
  void stick(const std::string& a) {
    std::lock_guard lk(m_);
    stuck_.push_back(a);
  }
  std::vector<std::string> calls() {
    std::lock_guard lk(m_);
    return calls_;
  }
  std::size_t commands() {
    std::size_t n = 0;
    for (const auto& c : calls()) n += c.rfind("read", 0) != 0;
    return n;
  }
  std::vector<TimePoint> read_times() {
    std::lock_guard lk(m_);
    return read_times_;
  }

 private:
  Result<void> command(const ValveAddress& a, ValveState s) {
    std::lock_guard lk(m_);
    calls_.push_back((s == ValveState::Open ? "open " : "close ") + a.value);
    if (fail_command_) return fail(*fail_command_, "command failed", "fake");
    if (std::find(stuck_.begin(), stuck_.end(), a.value) == stuck_.end()) hw_[a.value] = s;
    return {};
  }

  const Clock* clock_;
  std::mutex m_;
  std::map<std::string, ValveState> hw_;
  std::vector<std::string> calls_;
  std::vector<TimePoint> read_times_;
  std::vector<std::string> stuck_;
  std::optional<ErrorKind> fail_command_;
  std::optional<ErrorKind> fail_read_;
};

SwitchSpec valve(std::string name, std::string address, std::vector<std::string> interlocks = {},
                 std::vector<std::string> positive = {}) {
  SwitchSpec s;
  s.name = std::move(name);
  s.actuator = "act";
  s.address = ValveAddress{std::move(address)};
  s.interlocks = std::move(interlocks);
  s.positive_interlocks = std::move(positive);
  return s;
}

SwitchSpec manual(std::string name) {
  SwitchSpec s;
  s.name = std::move(name);
  s.kind = SwitchKind::ManualValve;
  return s;
}

SwitchSpec power_switch(std::string name, std::string address) {
  SwitchSpec s = valve(std::move(name), std::move(address));
  s.kind = SwitchKind::Switch;
  return s;
}

// Collects every event the manager publishes.
struct Recorder {
  explicit Recorder(SignalBus& bus)
      : changed_sub(bus.subscribe<ValveChanged>([this](const ValveChanged& e) {
          std::lock_guard lk(m);
          changed.push_back(e);
        })),
        failed_sub(bus.subscribe<ActuationFailed>([this](const ActuationFailed& e) {
          std::lock_guard lk(m);
          failed.push_back(e);
        })) {}

  std::mutex m;
  std::vector<ValveChanged> changed;
  std::vector<ActuationFailed> failed;
  SignalBus::Subscription changed_sub;
  SignalBus::Subscription failed_sub;
};

struct Fixture {
  explicit Fixture(std::vector<SwitchSpec> specs, bool refresh = true) {
    auto made = SwitchManager::create(
        std::move(specs), [this](const std::string& n) -> IValveActuator* { return n == "act" ? &act : nullptr; },
        {&clock, &bus});
    EXPECT_TRUE(made) << (made ? "" : made.error().what);
    if (made) mgr = std::move(*made);
    if (mgr && refresh) EXPECT_TRUE(mgr->refresh());
  }

  ManualClock clock;
  SignalBus bus;
  FakeActuator act{&clock};
  Recorder rec{bus};
  std::unique_ptr<SwitchManager> mgr;
};

ValveState st(const SwitchManager& m, std::string_view name) {
  auto s = m.state(name);
  return s ? *s : ValveState::Unknown;
}

}  // namespace

// ---- construction ----------------------------------------------------------

TEST(SwitchManager, CreateRejectsBadSpecsTogether) {
  FakeActuator act;
  auto lookup = [&](const std::string& n) -> IValveActuator* { return n == "act" ? &act : nullptr; };
  auto self = valve("A", "1", {"A"});
  auto dangling = valve("B", "2", {"nope"});
  auto no_actuator = valve("C", "3");
  no_actuator.actuator = "missing";
  auto both = valve("D", "4", {"B"}, {"B"});
  auto manual_interlock = manual("M");
  manual_interlock.interlocks = {"B"};
  auto r = SwitchManager::create({self, dangling, no_actuator, both, manual_interlock, valve("B", "5")}, lookup);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  for (const char* needle : {"itself", "unknown switch 'nope'", "missing", "both", "duplicate", "'M'"}) {
    EXPECT_NE(r.error().what.find(needle), std::string::npos) << needle << " in: " << r.error().what;
  }
}

TEST(SwitchManager, StartsUnknownUntilRefreshReadsHardware) {
  Fixture f({valve("A", "1"), valve("B", "2"), manual("M")}, /*refresh=*/false);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);
  f.act.set_hw("2", ValveState::Open);
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
  EXPECT_EQ(st(*f.mgr, "B"), ValveState::Open);
  EXPECT_EQ(st(*f.mgr, "M"), ValveState::Unknown);  // no hardware to ask
  EXPECT_EQ(f.rec.changed.size(), 2u);
}

TEST(SwitchManager, UnknownNameIsConfig) {
  Fixture f({valve("A", "1")});
  auto r = f.mgr->actuate("Z", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_FALSE(f.mgr->state("Z"));
  EXPECT_FALSE(f.mgr->lock("Z"));
}

// ---- happy path, settle, read-back -----------------------------------------

TEST(SwitchManager, ActuateCommandsReadsBackAndPublishes) {
  Fixture f({valve("A", "1")});
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Open);
  auto calls = f.act.calls();
  ASSERT_GE(calls.size(), 2u);
  EXPECT_EQ(calls[calls.size() - 2], "open 1");
  EXPECT_EQ(calls.back(), "read 1");
  ASSERT_FALSE(f.rec.changed.empty());
  EXPECT_EQ(f.rec.changed.back().valve, "A");
  EXPECT_EQ(f.rec.changed.back().state, ValveState::Open);
  EXPECT_EQ(f.rec.changed.back().ts, f.clock.now());

  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "op"));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
  EXPECT_TRUE(f.rec.failed.empty());
}

TEST(SwitchManager, WaitsSettleTimeBeforeReadBack) {
  auto a = valve("A", "1");
  a.settle = 1000ms;
  Fixture f({a});
  const auto start = f.clock.now();
  std::atomic<bool> done{false};
  std::thread t([&] {
    EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
    done = true;
  });
  // Commanded but not read back while the clock has not moved.
  for (int i = 0; i < 200 && f.act.commands() == 0; ++i) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(done);
  f.clock.advance(999ms);
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(done);
  f.clock.advance(1ms);
  t.join();
  auto reads = f.act.read_times();
  ASSERT_FALSE(reads.empty());
  EXPECT_GE(reads.back() - start, 1000ms);
}

TEST(SwitchManager, ReadBackMismatchIsProtocolAndRecordsHardwareState) {
  Fixture f({valve("A", "1")});
  f.act.stick("1");
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
  ASSERT_EQ(f.rec.failed.size(), 1u);
  EXPECT_EQ(f.rec.failed[0].valve, "A");
  EXPECT_EQ(f.rec.failed[0].error.kind, ErrorKind::Protocol);
}

TEST(SwitchManager, CommandFailureLeavesUnknownAndPublishes) {
  Fixture f({valve("A", "1")});
  f.act.fail_commands(ErrorKind::Timeout);
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);
  ASSERT_EQ(f.rec.failed.size(), 1u);
  EXPECT_EQ(f.rec.failed[0].error.kind, ErrorKind::Timeout);
}

TEST(SwitchManager, ReadBackFailureLeavesUnknown) {
  Fixture f({valve("A", "1")});
  f.act.fail_reads(ErrorKind::Io);
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);
  EXPECT_EQ(f.rec.failed.size(), 1u);
}

TEST(SwitchManager, RefreshReportsReadErrors) {
  Fixture f({valve("A", "1")});
  f.act.fail_reads(ErrorKind::NotConnected);
  auto r = f.mgr->refresh();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);
}

// ---- lock and owner --------------------------------------------------------

TEST(SwitchManager, LockedSwitchRefusesEveryoneAndSendsNothing) {
  Fixture f({valve("A", "1")});
  ASSERT_TRUE(f.mgr->lock("A"));
  const auto before = f.act.commands();
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(f.act.commands(), before);
  ASSERT_EQ(f.rec.failed.size(), 1u);
  EXPECT_EQ(f.rec.failed[0].error.kind, ErrorKind::Interlock);
  EXPECT_TRUE(f.mgr->info("A")->locked);

  ASSERT_TRUE(f.mgr->unlock("A"));
  EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
}

TEST(SwitchManager, OwnedSwitchAcceptsOnlyItsOwner) {
  Fixture f({valve("A", "1")});
  ASSERT_TRUE(f.mgr->claim("A", "experiment"));
  EXPECT_TRUE(f.mgr->claim("A", "experiment"));  // idempotent
  auto other = f.mgr->claim("A", "ui");
  ASSERT_FALSE(other);
  EXPECT_EQ(other.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(f.mgr->info("A")->owner, "experiment");

  auto r = f.mgr->actuate("A", SwitchOp::Open, "ui");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "experiment"));

  EXPECT_FALSE(f.mgr->release("A", "ui"));
  ASSERT_TRUE(f.mgr->release("A", "experiment"));
  EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "ui"));
}

// ---- interlocks ------------------------------------------------------------

TEST(SwitchManager, NegativeInterlockBlocksOpeningEitherSide) {
  Fixture f({valve("A", "1", {"B"}), valve("B", "2")});
  ASSERT_TRUE(f.mgr->actuate("B", SwitchOp::Open, "op"));
  const auto before = f.act.commands();
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("B"), std::string::npos);
  EXPECT_EQ(f.act.commands(), before);
  ASSERT_EQ(f.rec.failed.size(), 1u);
  EXPECT_EQ(f.rec.failed[0].valve, "A");

  ASSERT_TRUE(f.mgr->actuate("B", SwitchOp::Close, "op"));
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  // Reverse direction: B does not list A, but they may never be open together.
  auto rb = f.mgr->actuate("B", SwitchOp::Open, "op");
  ASSERT_FALSE(rb);
  EXPECT_EQ(rb.error().kind, ErrorKind::Interlock);
}

TEST(SwitchManager, UnknownPartnerBlocksOpening) {
  Fixture f({valve("A", "1", {"B"}), valve("B", "2")}, /*refresh=*/false);
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
}

TEST(SwitchManager, PositiveInterlockRequiresPrerequisitesOpen) {
  Fixture f({valve("A", "1", {}, {"B", "C"}), valve("B", "2"), valve("C", "3")});
  ASSERT_TRUE(f.mgr->actuate("B", SwitchOp::Open, "op"));
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("C"), std::string::npos);
  ASSERT_TRUE(f.mgr->actuate("C", SwitchOp::Open, "op"));
  EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
}

TEST(SwitchManager, ClosingIsNeverInterlocked) {
  Fixture f({valve("A", "1", {"B"}), valve("B", "2")});
  f.act.set_hw("1", ValveState::Open);
  f.act.set_hw("2", ValveState::Open);
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "op"));
}

TEST(SwitchManager, ManualValveRecordsOperatorReportAndInterlocks) {
  Fixture f({valve("A", "1", {"M"}), manual("M")});
  EXPECT_FALSE(f.mgr->actuate("A", SwitchOp::Open, "op"));  // M Unknown
  ASSERT_TRUE(f.mgr->actuate("M", SwitchOp::Close, "op"));
  EXPECT_EQ(st(*f.mgr, "M"), ValveState::Closed);
  EXPECT_EQ(f.rec.changed.back().valve, "M");
  EXPECT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  auto r = f.mgr->actuate("M", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
}

TEST(SwitchManager, SwitchesActuateWithoutInterlocks) {
  Fixture f({valve("A", "1"), power_switch("pump_power", "7")});
  ASSERT_TRUE(f.mgr->actuate("pump_power", SwitchOp::Open, "op"));
  EXPECT_EQ(f.act.hw("7"), ValveState::Open);
  EXPECT_EQ(f.mgr->info("pump_power")->kind, SwitchKind::Switch);
  EXPECT_EQ(f.mgr->list().size(), 2u);
  EXPECT_EQ(f.mgr->states().at("pump_power"), ValveState::Open);
}

// Two callers racing to open mutually interlocked valves: at most one wins.
TEST(SwitchManager, ConcurrentActuationsCannotBothOpenInterlockedPair) {
  for (int round = 0; round < 50; ++round) {
    Fixture f({valve("A", "1", {"B"}), valve("B", "2")});
    std::thread ta([&] { (void)f.mgr->actuate("A", SwitchOp::Open, "x"); });
    std::thread tb([&] { (void)f.mgr->actuate("B", SwitchOp::Open, "y"); });
    ta.join();
    tb.join();
    EXPECT_FALSE(f.act.hw("1") == ValveState::Open && f.act.hw("2") == ValveState::Open) << "round " << round;
  }
}

// ---- config ----------------------------------------------------------------

TEST(SwitchManager, FromConfigBuildsValvesAndManualValves) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.act]
kind = "proxr_relay"
transport = "bus"
[[valves]]
name = "A"
actuator = "act"
address = "1"
interlocks = ["B"]
settle_ms = 0
[[valves]]
name = "B"
actuator = "act"
address = "2"
positive_interlocks = ["M1"]
[[manual_valves]]
name = "M1"
description = "hand valve"
[[switches]]
name = "pump_power"
actuator = "act"
address = "9"
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  FakeActuator act;
  auto mgr = SwitchManager::from_config(
      *cfg, [&](const std::string& n) -> IValveActuator* { return n == "act" ? &act : nullptr; });
  ASSERT_TRUE(mgr) << mgr.error().what;
  auto list = (*mgr)->list();
  ASSERT_EQ(list.size(), 4u);
  EXPECT_EQ(list[0].name, "A");
  EXPECT_EQ(list[2].kind, SwitchKind::ManualValve);
  EXPECT_EQ(list[2].description, "hand valve");
  EXPECT_EQ(list[3].name, "pump_power");
  EXPECT_EQ(list[3].kind, SwitchKind::Switch);

  auto missing = SwitchManager::from_config(*cfg, [](const std::string&) -> IValveActuator* { return nullptr; });
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);
}

// End to end over the real driver and a simulated board.
TEST(SwitchManager, DrivesProxrRelayOverSimulatedBoard) {
  ProxrBoardSim board;
  TransportOptions o;
  o.name = "valve_bus";
  o.timeout = 50ms;
  auto t = SimTransport::hooked(board.hook(), o);
  ASSERT_TRUE(t->open());
  ProxrRelay relay("act", *t);
  SignalBus bus;
  Recorder rec(bus);
  auto made = SwitchManager::create(
      {valve("A", "9", {"B"}), valve("B", "10")},
      [&](const std::string& n) -> IValveActuator* { return n == "act" ? &relay : nullptr; }, {nullptr, &bus});
  ASSERT_TRUE(made);
  auto& mgr = **made;
  ASSERT_TRUE(mgr.refresh());
  ASSERT_TRUE(mgr.actuate("A", SwitchOp::Open, "op"));
  EXPECT_TRUE(board.energized(9));
  EXPECT_FALSE(mgr.actuate("B", SwitchOp::Open, "op"));
  EXPECT_FALSE(board.energized(10));
  ASSERT_TRUE(mgr.actuate("A", SwitchOp::Close, "op"));
  EXPECT_FALSE(board.energized(9));
  EXPECT_TRUE(rec.failed.size() == 1u && rec.failed[0].error.kind == ErrorKind::Interlock);
}
