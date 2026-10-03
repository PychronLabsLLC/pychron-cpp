// Isotopx NGX drivers (NgxLink, isotopx_ngx, ngx_valves) against the NGX
// simulator: the session, acquisition by events, replies interleaved with
// events, late replies, magnet moves during an integration, source
// parameters, valves sharing the link, reconnects, and a concurrency stress.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "pychron/devices/spectrometer/ngx.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/transport/link_transport.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

// Forwards to a SimTransport; can fail the next read with Io (a dropped
// connection) and tells the model about reopens so it greets again.
class FlakyTransport final : public Transport {
 public:
  FlakyTransport(Transport& inner, std::shared_ptr<NgxSimModel> model) : inner_(inner), model_(std::move(model)) {}
  const std::string& name() const override { return inner_.name(); }
  Result<void> open() override {
    {
      std::lock_guard lock(model_->mutex);
      model_->banner_pending = true;
    }
    ++opens;
    return inner_.open();
  }
  void close() override { inner_.close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override { return inner_.exchange(tx, rs, timeout); }
  Result<void> write(Bytes tx) override {
    if (before_write) before_write(std::string(tx.begin(), tx.end()));
    return inner_.write(std::move(tx));
  }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override {
    if (drop_next.exchange(false)) return fail(ErrorKind::Io, "connection reset");
    return inner_.read(std::move(rs), timeout);
  }
  Result<void> transaction(std::function<Result<void>()> body) override { return inner_.transaction(std::move(body)); }
  Health health() const override { return inner_.health(); }

  std::function<void(const std::string&)> before_write;  // set before connect()
  std::atomic<bool> drop_next{false};
  std::atomic<int> opens{0};

 private:
  Transport& inner_;
  std::shared_ptr<NgxSimModel> model_;
};

std::string uniq(const char* base) {
  static std::atomic<int> n{0};
  return std::string(base) + std::to_string(++n);
}

struct Ngx : ::testing::Test {
  ManualClock clock{TimePoint{} + 12h};
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
  std::this_thread::sleep_for(100ms);
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
  clock.advance(2s);
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
  clock.advance(2s);
  auto none = s->next(200ms);
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);  // two of three
  clock.advance(1s);
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
  clock.advance(2s);
  ASSERT_TRUE((*v)->open(ValveAddress{"7"}));
  EXPECT_EQ(count("StopAcq"), 2);  // connect + start: none from the valve
  clock.advance(3s);
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
  clock.advance(1s);
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
  clock.advance(7s);
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
  for (int i = 0; i < 200 && (*l)->stats().reconnects == 0; ++i) std::this_thread::sleep_for(10ms);
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
TEST_F(Ngx, AMoveRacingATriggerNeverLeavesAnIntegrationRunning) {
  auto s = make(*sim);
  ASSERT_TRUE(s->connect());
  ASSERT_TRUE(s->start());
  for (int i = 0; i < 300; ++i) {
    std::atomic<int> ready{0};
    auto go = [&] {
      ++ready;
      while (ready < 2) std::this_thread::yield();
    };
    std::thread trigger([&] {
      go();
      auto r = s->trigger();
      EXPECT_TRUE(r) << (r ? "" : r.error().what);
    });
    std::thread move([&] {
      go();
      EXPECT_TRUE(s->set(36.0 + i % 3));
    });
    trigger.join();
    move.join();
    (void)s->next(0ms);  // drop the cancellation the move queued
    // The trigger either was aborted or armed; settle it so the next round starts idle.
    ASSERT_TRUE(s->set(39.96));
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
  std::atomic<bool> done{false};
  std::thread ticker([&] {
    while (!done) {
      clock.advance(200ms);
      std::this_thread::sleep_for(2ms);
    }
  });
  std::thread valves([&] {
    for (int i = 0; i < 40; ++i) {
      EXPECT_TRUE((*v)->open(ValveAddress{std::to_string(i % 4)}));
      EXPECT_TRUE((*v)->close(ValveAddress{std::to_string(i % 4)}));
    }
  });
  std::thread magnet([&] {
    for (int i = 0; i < 10; ++i) {
      EXPECT_TRUE(s->set(36.0 + i % 5));
      std::this_thread::sleep_for(5ms);
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
  valves.join();
  magnet.join();
  done = true;
  ticker.join();
  EXPECT_GT(frames, 0);
  EXPECT_LE(cancelled, 10);  // at most one per move
  std::lock_guard lock(model->mutex);
  EXPECT_EQ(model->unbracketed_actuations, 0);
  EXPECT_FALSE(model->sab);
}
