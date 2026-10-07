// Isotopx NGX drivers (NgxLink, isotopx_ngx, ngx_valves) against the NGX
// simulator: the session, acquisition by events, replies interleaved with
// events, late replies, magnet moves during an integration, source
// parameters, valves sharing the link, reconnects, and a concurrency stress.
// Time is a VirtualClock shared by the simulator, its transport, the link and
// the drivers; nothing waits on the wall clock. (NgxLinkSteady, at the end,
// is the exception: the link on a SteadyClock, as on hardware.)

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/spectrometer/ngx.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/transport/link_transport.hpp"
#include "valve_conformance.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

// Forwards to a SimTransport; can fail the next read with Io (a dropped
// connection), refuse the next opens, and tells the model (when there is one)
// about reopens so it greets again.
class FlakyTransport final : public Transport {
 public:
  FlakyTransport(Transport& inner, std::shared_ptr<NgxSimModel> model) : inner_(inner), model_(std::move(model)) {}
  const std::string& name() const override { return inner_.name(); }
  Result<void> open() override {
    if (model_) {
      std::lock_guard lock(model_->mutex);
      model_->banner_pending = true;
    }
    ++opens;
    if (on_open) on_open();
    if (refuse_opens > 0) {
      --refuse_opens;
      return fail(ErrorKind::Io, "connection refused");
    }
    return inner_.open();
  }
  void close() override { inner_.close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override { return inner_.exchange(tx, rs, timeout); }
  Result<void> write(Bytes tx) override {
    if (before_write) before_write(std::string(tx.begin(), tx.end()));
    return inner_.write(std::move(tx));
  }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override {
    if (drop_next.exchange(false)) {
      if (on_drop) on_drop();
      return fail(ErrorKind::Io, "connection reset");
    }
    return inner_.read(std::move(rs), timeout);
  }
  Result<void> transaction(std::function<Result<void>()> body) override { return inner_.transaction(std::move(body)); }
  Health health() const override { return inner_.health(); }

  std::function<void(const std::string&)> before_write;  // set before connect()
  std::function<void()> on_open, on_drop;                // likewise; called on the reader
  std::atomic<bool> drop_next{false};
  std::atomic<int> refuse_opens{0};
  std::atomic<int> opens{0};

 private:
  Transport& inner_;
  std::shared_ptr<NgxSimModel> model_;
};

using pychron::testing::Crew;

std::string uniq(const char* base) {
  static std::atomic<int> n{0};
  return std::string(base) + std::to_string(++n);
}

struct Ngx : pychron::testing::VirtualTimeTest {
  VirtualClock clock;
  // The test's thread takes part: time moves only while it waits in the clock
  // (clock.sleep_for, a command, next()), and stands still otherwise.
  Clock::Participant main{clock, "test"};
  std::shared_ptr<NgxSimModel> model = [this] {
    auto m = std::make_shared<NgxSimModel>();
    m->clock = &clock;
    m->values = {1.5, 2.5, 3.5, 4.5};
    m->mass = 39.96;
    return m;
  }();
  std::unique_ptr<SimTransport> sim = [this] {
    TransportOptions o;
    o.name = "ngx";
    o.clock = &clock;
    auto t = SimTransport::hooked(ngx_sim_hook(model), o, ngx_sim_events(model));
    EXPECT_TRUE(t->open());
    return t;
  }();
  std::string link = uniq("ngx");

  toml::table options(const std::string& extra = "") const {
    const std::string timeout = extra.find("command_timeout_ms") == std::string::npos ? "command_timeout_ms = 1000\n" : "";
    auto parsed = toml::parse("channels = [\"H1\", \"AX\", \"L1\", \"CDD\"]\nlink = \"" + link + "\"\n" +
                              "user = \"pychron\"\npassword = \"s3cret\"\n" + timeout + extra);
    EXPECT_TRUE(parsed) << parsed.error().description();
    return parsed ? parsed.table() : toml::table{};
  }

  std::unique_ptr<NgxSpectrometer> make(Transport& t, const std::string& extra = "") {
    const auto o = options(extra);
    auto r = NgxSpectrometer::create(DriverArgs{"ngx", t, o, &clock});
    EXPECT_TRUE(r) << (r ? "" : r.error().what);
    return r ? std::move(*r) : nullptr;
  }

  std::vector<std::string> commands() {
    std::lock_guard lock(model->mutex);
    return model->commands;
  }
  int count(const std::string& prefix) {
    auto c = commands();
    return static_cast<int>(std::count_if(c.begin(), c.end(), [&](const std::string& s) { return s.starts_with(prefix); }));
  }

  // next() until a frame or error comes (the reader delivers asynchronously).
  Result<std::optional<Frame>> wait_frame(NgxSpectrometer& s) { return s.next(3s); }
};

}  // namespace

TEST_F(Ngx, TheSessionLogsInAndNeverShowsThePassword) {
  auto s = make(*sim);
  ASSERT_TRUE(s);
  ASSERT_TRUE(s->connect());
  {
    std::lock_guard lock(model->mutex);
    EXPECT_TRUE(model->logged_in);
    EXPECT_EQ(model->user, "pychron");
    EXPECT_EQ(model->password, "s3cret");
  }
  auto c = commands();
  ASSERT_GE(c.size(), 3u);
  EXPECT_EQ(c[0], "Login pychron,s3cret");
  EXPECT_EQ(c[1], "StopAcq");
  EXPECT_EQ(c[2], "SetAcqPeriod 1000");
  // Every command ends with the configured terminator, "\r" by default.
  for (const auto& tx : sim->written()) EXPECT_EQ(tx.back(), '\r');
  // The reader polls constantly; an idle link is not a failing transport.
  clock.sleep_for(100ms);
  EXPECT_EQ(sim->health().state, HealthState::Connected);
  EXPECT_EQ(sim->health().consecutive_failures, 0u);
}

TEST_F(Ngx, ARefusedLoginFailsTheConnect) {
  {
    std::lock_guard lock(model->mutex);
    model->login_reply = "E42";
  }
  auto s = make(*sim);
  auto r = s->connect();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
  EXPECT_NE(r.error().what.find("Login pychron,****"), std::string::npos) << r.error().what;
  EXPECT_EQ(r.error().what.find("s3cret"), std::string::npos) << r.error().what;
}

TEST_F(Ngx, AnIntegrationCompletesOnTheBufferedEvent) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->configure(2s));
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->trigger());
  ASSERT_TRUE(s->trigger());  // armed: no second StartAcq
  EXPECT_EQ(count("StartAcq"), 1);
  EXPECT_EQ(commands().back(), "StartAcq 2,NOM");
  // Nothing before the instrument has integrated.
  auto early = s->next(100ms);
  ASSERT_TRUE(early);
  EXPECT_FALSE(*early);
  clock.sleep_for(2s);
  auto f = wait_frame(*s);
  ASSERT_TRUE(f) << f.error().what;
  ASSERT_TRUE(*f);
  EXPECT_TRUE((*f)->integrated);
  EXPECT_EQ((*f)->span, 2s);
  // Channel order, though the wire carries them reversed.
  EXPECT_EQ((*f)->value("H1"), 1.5);
  EXPECT_EQ((*f)->value("CDD"), 4.5);
  EXPECT_EQ(s->acq_stats().completed, 1u);
  // The per-second ACQ lines did not complete it; the next trigger arms anew.
  ASSERT_TRUE(s->trigger());
  EXPECT_EQ(count("StartAcq"), 2);
}

TEST_F(Ngx, LastAcqCompletesOnTheNthEvent) {
  {
    std::lock_guard lock(model->mutex);
    model->emit_acq_b = false;
  }
  auto s = make(*sim, "completion = \"last_acq\"\n");
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->configure(3s));
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->trigger());
  clock.sleep_for(2s);
  auto none = s->next(200ms);
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);  // two of three
  clock.sleep_for(1s);
  auto f = wait_frame(*s);
  ASSERT_TRUE(f && *f);
  EXPECT_EQ((*f)->span, 3s);
}

TEST_F(Ngx, IntegrationTimesSnapToWhatTheNgxAccepts) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->configure(8s));  // nearest is 10
  ASSERT_TRUE(s->trigger());
  EXPECT_EQ(commands().back(), "StartAcq 10,NOM");
  EXPECT_FALSE(s->configure(0s));
}

TEST_F(Ngx, EventsInterleavedWithRepliesGoToTheRightPlace) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  {
    std::lock_guard lock(model->mutex);
    model->event_before_next_reply = "#EVENT:ACQ,NOM,SIM,SIM,12:00:00.0,9,9,9,9#\r\n";
  }
  // The stray event arrives just before the GETMASS reply: the reply still
  // answers GETMASS, and the event (nothing armed) is dropped.
  auto m = s->read();
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_DOUBLE_EQ(*m, 39.96);
  EXPECT_GE(s->acq_stats().dropped_events, 1u);
}

TEST_F(Ngx, ALateReplyIsNeverHandedToTheNextCommand) {
  auto s = make(*sim, "command_timeout_ms = 150\n");
  ASSERT_TRUE(s->connect());
  {
    std::lock_guard lock(model->mutex);
    model->params["IE"] = 4500;
    model->hold_replies = 1;  // the GSO reply comes late
  }
  auto hv = s->read_hv();
  ASSERT_FALSE(hv);
  EXPECT_EQ(hv.error().kind, ErrorKind::Timeout);
  model->release_held();
  // GETMASS waits out the owed reply; it gets its own answer, not "4500,4500".
  auto m = s->read();
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_DOUBLE_EQ(*m, 39.96);
  auto l = NgxLinkRegistry::global().find(link);
  ASSERT_TRUE(l);
  EXPECT_EQ((*l)->stats().late_replies, 1u);
}

TEST_F(Ngx, AMagnetMoveAbortsTheIntegrationAndDeflectsOnBigSteps) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->set(40.0));
  EXPECT_EQ(commands().back(), "SetMass 40.0,500");  // nothing commanded before: no deflect
  ASSERT_TRUE(s->trigger());
  ASSERT_TRUE(s->set(36.0));
  auto c = commands();
  ASSERT_GE(c.size(), 2u);
  EXPECT_EQ(c[c.size() - 2], "StopAcq");
  EXPECT_EQ(c.back(), "SetMass 36.0,500,deflect");
  auto f = s->next(1s);
  ASSERT_FALSE(f);
  EXPECT_EQ(f.error().kind, ErrorKind::Cancelled);
  EXPECT_NE(f.error().what.find("magnet move"), std::string::npos);
  ASSERT_TRUE(s->set(36.2));
  EXPECT_EQ(commands().back(), "SetMass 36.2,500");  // below the threshold
  EXPECT_FALSE(s->set(250.0));                      // outside 0..200
}

TEST_F(Ngx, ReadingTheMassWhileIntegratingSendsNothing) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->set(40.0));
  ASSERT_TRUE(s->trigger());
  const auto before = commands().size();
  auto m = s->read();
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(*m, 40.0);
  EXPECT_EQ(commands().size(), before);
}

TEST_F(Ngx, SourceParametersGoThroughSsoAndGso) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->set_hv(4500));
  EXPECT_EQ(commands().back(), "SSO IE, 4500.0");
  auto hv = s->read_hv();
  ASSERT_TRUE(hv);
  EXPECT_DOUBLE_EQ(*hv, 4500);
  ASSERT_TRUE(s->set_param(Custom{"YFocus"}, 1.5));
  EXPECT_EQ(commands().back(), "SSO YF, 1.5");
  auto yf = s->read_param(Custom{"YF"});
  ASSERT_TRUE(yf);
  EXPECT_DOUBLE_EQ(yf->setpoint, 1.5);
  auto trap = s->set_param(SourceParam::TrapCurrent, 200);
  ASSERT_FALSE(trap);
  EXPECT_EQ(trap.error().kind, ErrorKind::Config);
  EXPECT_FALSE(s->set_param(SourceParam::RotationQuad, 1));
  EXPECT_FALSE(s->set_hv(20000));
}

TEST_F(Ngx, ValvesShareTheLinkAndAreBracketedBySab) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  LinkTransport borrowed("ngx-line", link);
  const toml::table none;
  auto v = NgxValves::create(DriverArgs{"valves", borrowed, none, &clock});
  ASSERT_TRUE(v) << v.error().what;
  ASSERT_TRUE((*v)->connect());  // borrowed: the owner opened the session
  ASSERT_TRUE((*v)->open(ValveAddress{"3"}));
  auto c = commands();
  ASSERT_GE(c.size(), 3u);
  EXPECT_EQ(std::vector<std::string>(c.end() - 3, c.end()),
            (std::vector<std::string>{"SAB 1", "OpenValve 3", "SAB 0"}));
  auto state = (*v)->read(ValveAddress{"3"});
  ASSERT_TRUE(state);
  EXPECT_EQ(*state, ValveState::Open);
  {
    std::lock_guard lock(model->mutex);
    model->status_e00 = 1;  // pychron's oddity: E00 instead of a status
  }
  EXPECT_EQ(*(*v)->read(ValveAddress{"3"}), ValveState::Open);
  {
    std::lock_guard lock(model->mutex);
    model->status_e00 = 10;
  }
  auto garbage = (*v)->read(ValveAddress{"3"});
  ASSERT_FALSE(garbage);
  EXPECT_EQ(garbage.error().kind, ErrorKind::Protocol);  // never "closed"
  ASSERT_TRUE((*v)->close(ValveAddress{"3"}));
  std::lock_guard lock(model->mutex);
  EXPECT_EQ(model->unbracketed_actuations, 0);
  EXPECT_FALSE(model->sab);
}

TEST_F(Ngx, ValvesPassTheValveConformanceSuite) {
  // Silence and garbage are not exercised here: the link reconnects and logs
  // in again, which the tests above cover.
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  LinkTransport borrowed("ngx-line", link);
  const toml::table none;
  auto v = NgxValves::create(DriverArgs{"valves", borrowed, none, &clock});
  ASSERT_TRUE(v) << v.error().what;
  struct Rig final : pychron::test::ValveRig {
    IValveActuator* valves = nullptr;
    IValveActuator& actuator() override { return *valves; }
    ValveAddress first() override { return {"3"}; }
    ValveAddress second() override { return {"7"}; }
  } rig;
  rig.valves = v->get();
  pychron::test::expect_valve_conformance(rig);
}

TEST_F(Ngx, AValveActuationDuringAnIntegrationLeavesItAlone) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  LinkTransport borrowed("ngx-line", link);
  const toml::table none;
  auto v = NgxValves::create(DriverArgs{"valves", borrowed, none, &clock});
  ASSERT_TRUE(v);
  ASSERT_TRUE(s->configure(5s));
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->trigger());
  clock.sleep_for(2s);
  ASSERT_TRUE((*v)->open(ValveAddress{"7"}));
  EXPECT_EQ(count("StopAcq"), 2);  // connect + start: none from the valve
  clock.sleep_for(3s);
  auto f = wait_frame(*s);
  ASSERT_TRUE(f && *f);
  EXPECT_EQ(s->acq_stats().completed, 1u);
  EXPECT_EQ(s->acq_stats().aborted, 0u);
}

TEST_F(Ngx, LinksAreFoundByNameAndDeclaredOnce) {
  LinkTransport nowhere("t", uniq("absent"));
  const toml::table none;
  auto v = NgxValves::create(DriverArgs{"valves", nowhere, none, &clock});
  ASSERT_TRUE(v);
  auto r = (*v)->open(ValveAddress{"1"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
  EXPECT_NE(r.error().what.find("is not running"), std::string::npos);

  auto first = make(*sim);
  ASSERT_TRUE(first);
  const auto o = options();
  auto second = NgxSpectrometer::create(DriverArgs{"ngx2", *sim, o, &clock});
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().kind, ErrorKind::Config);
  EXPECT_NE(second.error().what.find("kind = \"link\""), std::string::npos);
  // The name frees up with its owner.
  first.reset();
  auto again = NgxSpectrometer::create(DriverArgs{"ngx3", *sim, o, &clock});
  EXPECT_TRUE(again);
}

TEST_F(Ngx, AWrongNumberOfValuesIsAProtocolError) {
  {
    std::lock_guard lock(model->mutex);
    model->values = {1, 2, 3};  // four channels configured
  }
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->trigger());
  clock.sleep_for(1s);
  auto f = wait_frame(*s);
  ASSERT_FALSE(f);
  EXPECT_EQ(f.error().kind, ErrorKind::Protocol);
}

TEST_F(Ngx, AnIntegrationThatNeverCompletesTimesOutAndStops) {
  {
    std::lock_guard lock(model->mutex);
    model->emit_acq_b = false;  // ACQ.B never comes
  }
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  ASSERT_TRUE(s->trigger());
  clock.sleep_for(7s);
  auto f = s->next(1s);
  ASSERT_FALSE(f);
  EXPECT_EQ(f.error().kind, ErrorKind::Timeout);
  EXPECT_NE(f.error().what.find("ACQ.B"), std::string::npos);
  EXPECT_EQ(commands().back(), "StopAcq");
  ASSERT_TRUE(s->trigger());  // idle again
}

TEST_F(Ngx, ADroppedConnectionReconnectsAndLogsInFirst) {
  FlakyTransport flaky(*sim, model);
  auto s = make(flaky);
  ASSERT_TRUE(s->connect());
  auto l = NgxLinkRegistry::global().find(link);
  ASSERT_TRUE(l);
  const auto session = (*l)->session();
  flaky.drop_next = true;
  // The reader reconnects in the background; the next command waits for it.
  for (int i = 0; i < 200 && (*l)->stats().reconnects == 0; ++i) clock.sleep_for(10ms);
  EXPECT_EQ((*l)->stats().reconnects, 1u);
  EXPECT_GT((*l)->session(), session);
  auto m = s->read();
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_EQ(count("Login"), 2);
  EXPECT_GE(flaky.opens.load(), 1);
}

// A move racing a trigger: whichever reaches the instrument first, the
// integration the trigger may have started is stopped. (The move's StopAcq
// going out first, then the StartAcq, left it running and the next StartAcq
// was E43.)
//
// Every thread here is a participant, so time moves only for the commands
// themselves: a round is a few of them, far less than the second an
// integration lasts, and one left running is still running when the round
// looks. (With time running free it would have finished by itself.)
TEST_F(Ngx, AMoveRacingATriggerNeverLeavesAnIntegrationRunning) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  for (int i = 0; i < 300; ++i) {
    const TimePoint round_start = clock.now();
    std::atomic<int> ready{0};
    auto go = [&] {
      ++ready;
      while (ready < 2) std::this_thread::yield();
    };
    Crew crew(clock);
    crew.start("trigger", [&] {
      go();
      auto r = s->trigger();
      EXPECT_TRUE(r) << (r ? "" : r.error().what);
    });
    crew.start("move", [&] {
      go();
      EXPECT_TRUE(s->set(36.0 + i % 3));
    });
    crew.join();
    (void)s->next(0ms);  // drop the cancellation the move queued
    // The trigger either was aborted or armed; settle it so the next round starts idle.
    ASSERT_TRUE(s->set(39.96));
    ASSERT_LT(clock.now() - round_start, 1s) << "round " << i << ": long enough for an integration to end by itself";
    bool running = false;
    {
      std::lock_guard lock(model->mutex);
      running = model->run && !model->run->done;
    }
    ASSERT_FALSE(running) << "round " << i << ": an integration was left running";
  }
}

// Acquisition, valves and magnet moves at once, for the thread sanitizer.
TEST_F(Ngx, ConcurrentAcquisitionValvesAndMovesStayConsistent) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  LinkTransport borrowed("ngx-line", link);
  const toml::table none;
  auto v = NgxValves::create(DriverArgs{"valves", borrowed, none, &clock});
  ASSERT_TRUE(v);
  ASSERT_TRUE(s->start());
  Crew crew(clock);
  crew.start("valves", [&] {
    for (int i = 0; i < 40; ++i) {
      EXPECT_TRUE((*v)->open(ValveAddress{std::to_string(i % 4)}));
      EXPECT_TRUE((*v)->close(ValveAddress{std::to_string(i % 4)}));
    }
  });
  crew.start("magnet", [&] {
    for (int i = 0; i < 10; ++i) {
      EXPECT_TRUE(s->set(36.0 + i % 5));
      clock.sleep_for(500ms);
    }
  });
  int frames = 0, cancelled = 0;  // a move aborts the integration it lands in
  for (int i = 0; i < 30; ++i) {
    auto armed = s->trigger();
    EXPECT_TRUE(armed) << (armed ? "" : armed.error().what);
    if (!armed) break;  // the threads below still get joined
    auto f = s->next(2s);
    if (f && *f) ++frames;
    else if (!f && f.error().kind == ErrorKind::Cancelled) ++cancelled;
  }
  crew.join();
  EXPECT_GT(frames, 0);
  EXPECT_LE(cancelled, 10);  // at most one per move
  std::lock_guard lock(model->mutex);
  EXPECT_EQ(model->unbracketed_actuations, 0);
  EXPECT_FALSE(model->sab);
}

// --- the link's own time --------------------------------------------------

namespace {

using pychron::testing::await_waiters;

// The link over a peer that never says anything: no banner, no reply, no
// event. A read of it does not wait (it has no event source), so the only
// clock time that passes is what the link itself waits for.
struct NgxLinkVirtual : pychron::testing::VirtualTimeTest {
  VirtualClock clock;
  Clock::Participant main{clock, "test"};
  std::unique_ptr<SimTransport> silent = [this] {
    TransportOptions o;
    o.name = "ngx";
    o.clock = &clock;
    auto t = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, o);
    EXPECT_TRUE(t->open());
    return t;
  }();
};

}  // namespace

TEST_F(NgxLinkVirtual, CommandTimeoutIsClockTime) {
  NgxLinkOptions o;
  o.command_timeout = 10s;
  NgxLink link(*silent, o, clock);
  ASSERT_TRUE(link.connect());
  const TimePoint kStart = clock.now();
  const auto real_start = std::chrono::steady_clock::now();

  auto r = link.ask("GETMASS");

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(clock.now(), kStart + o.command_timeout);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

TEST_F(NgxLinkVirtual, ReconnectBackoffIsClockTime) {
  FlakyTransport flaky(*silent, nullptr);
  std::mutex mutex;
  std::optional<TimePoint> dropped;
  std::vector<TimePoint> opened;
  flaky.on_drop = [&] {
    std::lock_guard lock(mutex);
    dropped = clock.now();
  };
  flaky.on_open = [&] {
    std::lock_guard lock(mutex);
    opened.push_back(clock.now());
  };
  NgxLinkOptions o;
  o.command_timeout = 60s;  // how long connect() waits for a link that is coming back
  o.reconnect_min = 2s;
  o.reconnect_max = 60s;
  NgxLink link(flaky, o, clock);
  ASSERT_TRUE(link.connect());
  const TimePoint kStart = clock.now();
  // Two waiters: the link's reader asleep between two reads, and the
  // transport's worker idle.
  ASSERT_TRUE(await_waiters(clock, 2));
  const auto real_start = std::chrono::steady_clock::now();

  // The connection drops and the peer is down for the first two opens.
  flaky.refuse_opens = 2;
  flaky.drop_next = true;
  clock.sleep_for(o.read_timeout + 1ms);  // past the reader's next read
  ASSERT_FALSE(link.up());
  ASSERT_TRUE(link.connect());  // returns when the reader has the link up again

  std::lock_guard lock(mutex);
  ASSERT_TRUE(dropped);
  EXPECT_EQ(*dropped, kStart + o.read_timeout);
  // 2 s, then 4 s, then 8 s: the third open succeeds.
  EXPECT_EQ(opened, (std::vector<TimePoint>{*dropped + 2s, *dropped + 6s, *dropped + 14s}));
  EXPECT_EQ(clock.now(), *dropped + 14s);
  EXPECT_TRUE(link.up());
  EXPECT_EQ(link.stats().reconnects, 1u);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

// --- the link on real time --------------------------------------------------

namespace {

// The hardware configuration: a SteadyClock. The timeouts are real and short;
// what is checked is that they are kept and that nothing hangs.
struct NgxLinkSteady : pychron::testing::VirtualTimeTest {
  SteadyClock clock;
  std::shared_ptr<NgxSimModel> model = [this] {
    auto m = std::make_shared<NgxSimModel>();
    m->clock = &clock;
    m->mass = 39.96;
    return m;
  }();
  TransportOptions transport_options() const {
    TransportOptions o;
    o.name = "ngx";
    o.clock = &clock;
    return o;
  }
  std::unique_ptr<SimTransport> open(std::unique_ptr<SimTransport> t) {
    EXPECT_TRUE(t->open());
    return t;
  }
  // The simulated controller, and a peer that never says anything.
  std::unique_ptr<SimTransport> sim = open(SimTransport::hooked(ngx_sim_hook(model), transport_options(), ngx_sim_events(model)));
  std::unique_ptr<SimTransport> silent = open(SimTransport::hooked([](const Bytes&) { return Bytes{}; }, transport_options()));

  static NgxLinkOptions short_timeouts() {
    NgxLinkOptions o;
    o.command_timeout = 50ms;
    o.banner_timeout = 50ms;
    return o;
  }
};

}  // namespace

TEST_F(NgxLinkSteady, ConnectsAndAnswersACommand) {
  NgxLinkOptions o;
  o.command_timeout = 5s;  // an upper limit only: the reply comes at once
  NgxLink link(*sim, o, clock);
  ASSERT_TRUE(link.connect());
  EXPECT_TRUE(link.up());
  EXPECT_EQ(link.session(), 1u);
  auto r = link.ask("GETMASS");
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_DOUBLE_EQ(std::stod(*r), 39.96);
  EXPECT_EQ(link.stats().replies_dropped, 0u);
}

TEST_F(NgxLinkSteady, ACommandToASilentPeerTimesOutInRealTime) {
  const NgxLinkOptions o = short_timeouts();
  NgxLink link(*silent, o, clock);
  ASSERT_TRUE(link.connect());
  const auto real_start = std::chrono::steady_clock::now();

  auto r = link.ask("GETMASS");

  const auto took = std::chrono::steady_clock::now() - real_start;
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_GE(took, 40ms);
  EXPECT_LT(took, 5s);
}

// The link goes while a command's reply is still outstanding: the command
// timed out, the reply is owed for late_reply_window, and the reader is in
// its read. The destructor waits for the reader and for nothing else.
TEST_F(NgxLinkSteady, TeardownWithAReplyOutstandingReturns) {
  {
    std::lock_guard lock(model->mutex);
    model->hold_replies = 1;
  }
  NgxLinkOptions o = short_timeouts();
  o.late_reply_window = 60s;
  auto link = std::make_optional<NgxLink>(*sim, o, clock);
  ASSERT_TRUE(link->connect());
  auto r = link->ask("GETMASS");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  ASSERT_TRUE(link->up());
  const auto real_start = std::chrono::steady_clock::now();

  link.reset();

  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}
