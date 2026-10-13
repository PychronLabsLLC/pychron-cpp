// SwitchManager: lock, owner, interlocks, settle, read-back and events,
// against an in-memory actuator, plus one run over the real ProXR driver.

#include "pychron/systems/switch_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/devices/proxr_relay.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "virtual_time.hpp"

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

  // Each read_many call's addresses, then the default per-address reads.
  std::vector<Result<ValveState>> read_many(const std::vector<ValveAddress>& addresses) override {
    {
      std::lock_guard lk(m_);
      std::vector<std::string> batch;
      for (const auto& a : addresses) batch.push_back(a.value);
      batches_.push_back(std::move(batch));
    }
    return IValveActuator::read_many(addresses);
  }
  std::vector<std::vector<std::string>> batches() {
    std::lock_guard lk(m_);
    return batches_;
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
    for (const auto& c : calls()) n += !c.starts_with("read");
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
  std::vector<std::vector<std::string>> batches_;
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
        {&clock, &bus, [this] { return wall; }});
    EXPECT_TRUE(made) << (made ? "" : made.error().what);
    if (made) mgr = std::move(*made);
    if (mgr && refresh) {
      EXPECT_TRUE(mgr->refresh());
    }
  }

  ManualClock clock;
  systems::WallTime wall{1'000'000s};  // SwitchStats times; a test moves it by hand
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

namespace {

struct SwitchManagerVirtual : pychron::testing::VirtualTimeTest {};

}  // namespace

// The same settle on a VirtualClock: the second passes on the clock and
// costs no real time.
TEST_F(SwitchManagerVirtual, ActuationDelayIsClockTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  SignalBus bus;
  FakeActuator act{&clock};
  auto a = valve("A", "1");
  a.settle = 1000ms;
  SwitchManager::Options options;
  options.clock = &clock;
  options.bus = &bus;
  auto mgr = SwitchManager::create({a}, [&](const std::string&) -> IValveActuator* { return &act; }, options);
  ASSERT_TRUE(mgr) << mgr.error().what;
  const auto start = clock.now();
  const auto real_start = std::chrono::steady_clock::now();

  ASSERT_TRUE((*mgr)->actuate("A", SwitchOp::Open, "op"));

  auto reads = act.read_times();
  ASSERT_FALSE(reads.empty());
  EXPECT_EQ(reads.back() - start, 1000ms);
  EXPECT_EQ(clock.now() - start, 1000ms);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

// An actuation holds the manager through its settle, which is clock time. A
// refresh that arrives meanwhile waits for it through the clock: blocked any
// other way it looks runnable, time stands, and the settle never ends.
TEST_F(SwitchManagerVirtual, ARefreshWaitsForAnActuationWithoutStallingTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  SignalBus bus;
  FakeActuator act{&clock};
  auto a = valve("A", "1");
  a.settle = 3000ms;
  SwitchManager::Options options;
  options.clock = &clock;
  options.bus = &bus;
  auto mgr = SwitchManager::create({a}, [&](const std::string&) -> IValveActuator* { return &act; }, options);
  ASSERT_TRUE(mgr) << mgr.error().what;
  const auto start = clock.now();
  const auto real_start = std::chrono::steady_clock::now();

  TimePoint actuated{}, refreshed{};
  bool actuate_ok = false, refresh_ok = false;
  pychron::testing::Crew crew(clock);
  crew.start("actuate", [&] {
    actuate_ok = (*mgr)->actuate("A", SwitchOp::Open, "op").has_value();
    actuated = clock.now();
  });
  ASSERT_TRUE(pychron::testing::await_waiters(clock, 1));  // the actuation, settling
  ASSERT_EQ(act.commands(), 1u);
  crew.start("refresh", [&] {
    refresh_ok = (*mgr)->refresh().has_value();
    refreshed = clock.now();
  });
  crew.join();

  EXPECT_TRUE(actuate_ok);
  EXPECT_TRUE(refresh_ok);
  EXPECT_EQ(actuated, start + 3000ms);
  EXPECT_EQ(refreshed, start + 3000ms);
  // The read-back after the settle, then the refresh's: none in between.
  EXPECT_EQ(act.read_times(), (std::vector<TimePoint>{start + 3000ms, start + 3000ms}));
  EXPECT_EQ(clock.now(), start + 3000ms);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

// With no wall function given, the times a switch's history is kept in (when
// it was last actuated, since when it has been in its state) are the clock's
// calendar time, to the second: under a simulated clock, simulated time.
TEST(SwitchManager, LockTimeIsTheClocksWallTime) {
  const pychron::WallTime epoch = pychron::WallTime{} + 1'700'000'000s + 250ms;
  ManualClock clock{TimePoint{}, epoch};
  SignalBus bus;
  FakeActuator act{&clock};
  SwitchManager::Options options;
  options.clock = &clock;
  options.bus = &bus;
  auto mgr = SwitchManager::create({valve("A", "1")}, [&](const std::string&) -> IValveActuator* { return &act; },
                                   options);
  ASSERT_TRUE(mgr) << mgr.error().what;
  ASSERT_TRUE((*mgr)->refresh());

  clock.advance(90s + 400ms);
  ASSERT_TRUE((*mgr)->actuate("A", SwitchOp::Open, "op"));

  // epoch + elapsed is 1 700 000 090.65 s: floored, not rounded.
  const systems::WallTime expected{1'700'000'090s};
  const SwitchStats stats = (*mgr)->info("A")->stats;
  EXPECT_EQ(stats.last_actuation, expected);
  EXPECT_EQ(stats.since, expected);
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

// Commands carried out and commands that failed are counted; a refusal sends
// nothing, counts as neither, and is counted on its own.
TEST(SwitchManager, StatsCountCommandsCarriedOutAndFailures) {
  Fixture f({valve("A", "1"), valve("B", "2", {"A"}), manual("M")});
  EXPECT_EQ(f.mgr->info("A")->stats, SwitchStats{});
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));  // sent again, though already open
  ASSERT_FALSE(f.mgr->actuate("B", SwitchOp::Open, "op"));  // interlocked: refused
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "op"));
  ASSERT_TRUE(f.mgr->actuate("M", SwitchOp::Open, "op"));
  f.wall += 60s;
  f.act.stick("1");
  ASSERT_FALSE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  f.act.fail_commands(ErrorKind::Timeout);
  ASSERT_FALSE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  const SwitchStats a = f.mgr->info("A")->stats;
  EXPECT_EQ(a.opens, 2);
  EXPECT_EQ(a.closes, 1);
  EXPECT_EQ(a.failures, 2);
  EXPECT_EQ(a.refusals, 0);
  EXPECT_EQ(a.last_actuation, systems::WallTime{1'000'000s});  // a failure is not an actuation
  SwitchStats refused;
  refused.refusals = 1;
  EXPECT_EQ(f.mgr->info("B")->stats, refused);
  ASSERT_TRUE(f.mgr->lock("A"));
  ASSERT_FALSE(f.mgr->actuate("A", SwitchOp::Close, "op"));  // locked: refused
  EXPECT_EQ(f.mgr->info("A")->stats.refusals, 1);
  EXPECT_EQ(f.mgr->info("A")->stats.failures, 2);
  EXPECT_EQ(f.mgr->info("M")->stats.opens, 1);
}

// `since` is when the recorded state last changed, and the time open adds up
// over spells that have ended. A state that comes out of Unknown is a
// reading: when the valve got there is not known, and is not made up.
TEST(SwitchManager, StatsTimeTheStateAndAddUpTheTimeOpen) {
  Fixture f({valve("A", "1")});
  EXPECT_FALSE(f.mgr->info("A")->stats.since);  // first read back closed
  f.wall += 10s;
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  EXPECT_EQ(f.mgr->info("A")->stats.since, systems::WallTime{1'000'010s});
  f.wall += 5s;
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));  // no change: the spell goes on
  EXPECT_EQ(f.mgr->info("A")->stats.since, systems::WallTime{1'000'010s});
  EXPECT_EQ(f.mgr->info("A")->stats.last_actuation, systems::WallTime{1'000'015s});
  f.wall += 25s;
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "op"));
  EXPECT_EQ(f.mgr->info("A")->stats.since, systems::WallTime{1'000'040s});
  EXPECT_EQ(f.mgr->info("A")->stats.open_time, 30s);

  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  f.wall += 100s;
  f.act.fail_commands(ErrorKind::Timeout);
  ASSERT_FALSE(f.mgr->actuate("A", SwitchOp::Close, "op"));  // now Unknown: the open spell ended here
  EXPECT_EQ(f.mgr->info("A")->stats.open_time, 130s);
  f.act.fail_commands(std::nullopt);
  f.wall += 7s;
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_NE(st(*f.mgr, "A"), ValveState::Unknown);
  EXPECT_FALSE(f.mgr->info("A")->stats.since);
}

TEST(SwitchManager, SeedStatsReplacesTheHistory) {
  Fixture f({valve("A", "1")});
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "op"));
  SwitchStats kept;
  kept.opens = 1200;
  kept.closes = 1199;
  kept.since = systems::WallTime{500s};
  kept.open_time = 3600s;
  f.mgr->seed_stats("A", kept);
  f.mgr->seed_stats("nope", kept);
  EXPECT_EQ(f.mgr->info("A")->stats, kept);
  f.wall += 10s;
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "op"));
  EXPECT_EQ(f.mgr->info("A")->stats.closes, 1200);
  EXPECT_EQ(f.mgr->info("A")->stats.open_time, 3600s + (systems::WallTime{1'000'010s} - systems::WallTime{500s}));
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
  EXPECT_NE(r.error().what.find('B'), std::string::npos);
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

// Which of two open partners is named is the one listed first, every run: not
// whichever the allocator happened to put at the lower address.
TEST(SwitchManager, TheInterlockNamedIsTheFirstListed) {
  Fixture f({valve("A", "1", {"C", "B"}), valve("B", "2"), valve("C", "3")});
  ASSERT_TRUE(f.mgr->actuate("B", SwitchOp::Open, "op"));
  ASSERT_TRUE(f.mgr->actuate("C", SwitchOp::Open, "op"));
  auto r = f.mgr->actuate("A", SwitchOp::Open, "op");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("interlocked with 'C'"), std::string::npos) << r.error().what;
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
  EXPECT_NE(r.error().what.find('C'), std::string::npos);
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

// ---- wiring: inverted, state_source, verify (plan 2026-10-05, task 0.2) -----

namespace {

// "act" and a second device "src" that only reads.
struct WiredFixture {
  explicit WiredFixture(std::vector<SwitchSpec> specs) {
    auto made = SwitchManager::create(
        std::move(specs),
        [this](const std::string& n) -> IValveActuator* {
          return n == "act" ? &act : n == "src" ? &src : nullptr;
        },
        {&clock, &bus});
    EXPECT_TRUE(made) << (made ? "" : made.error().what);
    if (made) mgr = std::move(*made);
  }

  ManualClock clock;
  SignalBus bus;
  FakeActuator act{&clock};
  FakeActuator src{&clock};
  Recorder rec{bus};
  std::unique_ptr<SwitchManager> mgr;
};

SwitchSpec inverted(SwitchSpec s) {
  s.inverted = true;
  return s;
}

SwitchSpec sourced(SwitchSpec s, std::string address, bool source_inverted = false) {
  s.state_source = StateSource{"src", ValveAddress{std::move(address)}, source_inverted};
  return s;
}

SwitchSpec unverified(SwitchSpec s) {
  s.verify = false;
  return s;
}

}  // namespace

TEST(SwitchManagerWiring, AnInvertedValveIsOpenedWithCloseAndRecordedAsTheValve) {
  WiredFixture f({inverted(valve("A", "1"))});
  f.act.set_hw("1", ValveState::Open);  // channel on: the valve is closed
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);

  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "me"));
  EXPECT_EQ(f.act.hw("1"), ValveState::Closed);  // the channel was closed
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Open);
  EXPECT_EQ(f.act.calls(), (std::vector<std::string>{"read 1", "close 1", "read 1"}));
  ASSERT_FALSE(f.rec.changed.empty());
  EXPECT_EQ(f.rec.changed.back().state, ValveState::Open);

  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "me"));
  EXPECT_EQ(f.act.hw("1"), ValveState::Open);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
}

TEST(SwitchManagerWiring, AStateSourceIsReadInsteadOfTheActuator) {
  WiredFixture f({sourced(valve("A", "101"), "102")});
  f.src.set_hw("102", ValveState::Open);
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Open);
  EXPECT_TRUE(f.act.calls().empty());  // refresh asked the source only

  // The command goes to the actuator; the read-back to the source, which
  // here has not followed: the manager says what the source says.
  auto r = f.mgr->actuate("A", SwitchOp::Close, "me");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(f.act.calls(), (std::vector<std::string>{"close 101"}));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Open);

  f.src.set_hw("102", ValveState::Closed);
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "me"));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
}

TEST(SwitchManagerWiring, AnInvertedStateSourceIsReadInverted) {
  // The valve's own inversion is its channel's; the source has its own.
  WiredFixture f({sourced(inverted(valve("A", "101")), "102", /*source_inverted=*/true)});
  f.src.set_hw("102", ValveState::Closed);  // input off: the valve is open
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Open);
  f.src.set_hw("102", ValveState::Open);
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "me"));
  EXPECT_EQ(f.act.calls(), (std::vector<std::string>{"open 101"}));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
}

TEST(SwitchManagerWiring, RefreshAsksEachReadBackDeviceOnceForAllItsSwitches) {
  // One read_many per device, in config order, so a PLC reads its coils in
  // runs (plan 2026-10-05, A7); inversions still apply per switch.
  WiredFixture f({valve("A", "1"), sourced(valve("B", "2"), "12"), inverted(valve("C", "3")),
                  unverified(valve("D", "4")), sourced(valve("E", "5"), "15", /*source_inverted=*/true)});
  f.act.set_hw("1", ValveState::Open);
  f.act.set_hw("3", ValveState::Open);
  f.src.set_hw("12", ValveState::Open);
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(f.act.batches(), (std::vector<std::vector<std::string>>{{"1", "3"}}));
  EXPECT_EQ(f.src.batches(), (std::vector<std::vector<std::string>>{{"12", "15"}}));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Open);
  EXPECT_EQ(st(*f.mgr, "B"), ValveState::Open);
  EXPECT_EQ(st(*f.mgr, "C"), ValveState::Closed);
  EXPECT_EQ(st(*f.mgr, "E"), ValveState::Open);  // source reads closed, inverted
  // Published in config order.
  std::vector<std::string> order;
  for (const auto& c : f.rec.changed) order.push_back(c.valve);
  EXPECT_EQ(order, (std::vector<std::string>{"A", "B", "C", "E"}));
}

TEST(SwitchManagerWiring, AnUnverifiedValveRecordsWhatItWasToldAndIsNeverRead) {
  WiredFixture f({unverified(valve("A", "1")), valve("B", "2", {"A"})});
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);  // nothing to read yet
  EXPECT_EQ(st(*f.mgr, "B"), ValveState::Closed);
  // Unknown blocks its interlock partner, as for any valve.
  EXPECT_FALSE(f.mgr->actuate("B", SwitchOp::Open, "me"));

  f.act.stick("1");  // the hardware ignores it; nobody can tell
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Close, "me"));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);
  ASSERT_TRUE(f.mgr->refresh());
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Closed);  // left as commanded
  for (const auto& c : f.act.calls()) EXPECT_NE(c, "read 1");
  ASSERT_TRUE(f.mgr->actuate("B", SwitchOp::Open, "me"));  // the commanded state holds the interlock
  EXPECT_EQ(f.mgr->info("A")->stats.closes, 1);
}

TEST(SwitchManagerWiring, AnUnverifiedValveWhoseCommandFailsIsUnknown) {
  WiredFixture f({unverified(valve("A", "1"))});
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "me"));
  f.act.fail_commands(ErrorKind::Io);
  EXPECT_FALSE(f.mgr->actuate("A", SwitchOp::Close, "me"));
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);
}

TEST(SwitchManagerWiring, ACloseThatCannotBeReadBackFails) {
  // Legacy passed this: a failed read on close was taken as closed.
  WiredFixture f({valve("A", "1")});
  ASSERT_TRUE(f.mgr->refresh());
  ASSERT_TRUE(f.mgr->actuate("A", SwitchOp::Open, "me"));
  f.act.fail_reads(ErrorKind::Timeout);
  auto r = f.mgr->actuate("A", SwitchOp::Close, "me");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(st(*f.mgr, "A"), ValveState::Unknown);
}

TEST(SwitchManagerWiring, CreateRejectsWiringThatCannotWork) {
  FakeActuator act;
  auto lookup = [&](const std::string& n) -> IValveActuator* { return n == "act" ? &act : nullptr; };
  auto hand = manual("M");
  hand.inverted = true;
  auto both = unverified(sourced(valve("B", "2"), "9"));
  both.state_source->driver = "act";
  auto nowhere = sourced(valve("C", "3"), "9");  // "src" does not resolve here
  auto r = SwitchManager::create({valve("A", "1"), hand, both, nowhere}, lookup);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  for (const char* needle : {"manual valve has no actuator", "cannot have a state_source", "state_source 'src'"}) {
    EXPECT_NE(r.error().what.find(needle), std::string::npos) << needle << " in: " << r.error().what;
  }
}

TEST(SwitchManagerWiring, FromConfigCarriesTheWiring) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.act]
kind = "proxr_relay"
transport = "bus"
[drivers.src]
kind = "proxr_relay"
transport = "bus"
[[valves]]
name = "A"
actuator = "act"
address = "1"
inverted = true
state_source = { driver = "src", address = 7, inverted = true }
[[valves]]
name = "B"
actuator = "act"
address = "2"
verify = false
[[switches]]
name = "pump_power"
actuator = "act"
address = "9"
inverted = true
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ASSERT_TRUE(cfg->valves[0].state_source);
  EXPECT_EQ(cfg->valves[0].state_source->address, "7");
  EXPECT_TRUE(cfg->switches[0].inverted);

  FakeActuator act, src;
  auto mgr = SwitchManager::from_config(*cfg, [&](const std::string& n) -> IValveActuator* {
    return n == "act" ? &act : n == "src" ? &src : nullptr;
  });
  ASSERT_TRUE(mgr) << mgr.error().what;
  src.set_hw("7", ValveState::Closed);  // inverted input: the valve is open
  ASSERT_TRUE((*mgr)->refresh());
  EXPECT_EQ((*mgr)->state("A").value(), ValveState::Open);
  EXPECT_EQ((*mgr)->state("B").value(), ValveState::Unknown);
  act.set_hw("9", ValveState::Open);
  ASSERT_TRUE((*mgr)->refresh());
  EXPECT_EQ((*mgr)->state("pump_power").value(), ValveState::Closed);
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
