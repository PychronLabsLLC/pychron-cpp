// The thermo_qtegra driver through the Spectrometer facade and ScanService,
// over its simulated wire (QtegraSimModel behind a hooked SimTransport that
// the assembler is handed in place of the configured transport). Time is a
// ManualClock pumped by a helper thread that also drives the Scheduler, so
// nothing waits on the wall clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kDir(PYCHRON_EXAMPLE_CONFIGS_DIR);
const std::filesystem::path kQtegra = kDir / "spectrometer.qtegra.toml";
const std::filesystem::path kSimIntegrated = kDir / "spectrometer.sim-integrated.toml";

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

class Pump {
 public:
  Pump(ManualClock& clock, Scheduler& scheduler) : clock_(clock), scheduler_(scheduler) {
    thread_ = std::thread([this] {
      while (!done_) {
        clock_.advance(20ms);
        scheduler_.run_pending();
        std::this_thread::sleep_for(200us);
      }
    });
  }
  ~Pump() {
    done_ = true;
    thread_.join();
  }

 private:
  ManualClock& clock_;
  Scheduler& scheduler_;
  std::atomic<bool> done_{false};
  std::thread thread_;
};

// What a test shares with the transport the Spectrometer owns.
struct Link {
  std::atomic<bool> down{false};  // every exchange fails with Io (a dropped connection)
  std::atomic<int> opens{0};      // open() calls, the assembler's included

  // Gate: while `hold` is set a GetData exchange stops here, before the wire.
  std::mutex mutex;
  std::condition_variable cv;
  bool hold = false;
  bool held = false;  // a GetData is waiting at the gate

  void hold_get_data() {
    std::lock_guard lock(mutex);
    hold = true;
  }
  bool wait_held() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 10s, [&] { return held; });
  }
  void release() {
    {
      std::lock_guard lock(mutex);
      hold = false;
    }
    cv.notify_all();
  }
};

// Owns the simulated wire. While the link is down an exchange fails before
// reaching it; reopening still succeeds.
class LinkTransport final : public Transport {
 public:
  LinkTransport(std::unique_ptr<SimTransport> inner, std::shared_ptr<Link> link)
      : inner_(std::move(inner)), link_(std::move(link)) {}

  const std::string& name() const override { return inner_->name(); }
  Result<void> open() override {
    ++link_->opens;
    return inner_->open();
  }
  void close() override { inner_->close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override {
    if (link_->down) return fail(ErrorKind::Io, "connection reset", inner_->name());
    if (to_string(tx).starts_with("GetData")) {
      std::unique_lock lock(link_->mutex);
      if (link_->hold) {
        link_->held = true;
        link_->cv.notify_all();
        link_->cv.wait(lock, [&] { return !link_->hold; });
        link_->held = false;
      }
    }
    return inner_->exchange(std::move(tx), std::move(rs), timeout);
  }
  Result<void> write(Bytes tx) override { return inner_->write(std::move(tx)); }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override { return inner_->read(std::move(rs), timeout); }
  Result<void> transaction(std::function<Result<void>()> body) override {
    return inner_->transaction(std::move(body));
  }
  Health health() const override { return inner_->health(); }

 private:
  std::unique_ptr<SimTransport> inner_;
  std::shared_ptr<Link> link_;
};

// Consecutive readings are one period apart. The poll that reads a frame runs
// on the first pump tick (20 ms) at or after the frame is due, so each
// timestamp is at most one tick late.
void expect_cadence(const std::vector<IntensityReading>& readings, double period_s) {
  for (std::size_t i = 1; i < readings.size(); ++i) {
    EXPECT_NEAR(seconds(readings[i].reading.ts - readings[i - 1].reading.ts), period_s, 0.020 + 1e-9) << i;
  }
}

// Bus events of one type, collected on whichever thread publishes them.
template <class E>
class Collector {
 public:
  explicit Collector(SignalBus& bus) {
    sub_ = bus.subscribe<E>([this](const E& e) {
      {
        std::lock_guard lock(mutex_);
        events_.push_back(e);
      }
      cv_.notify_all();
    });
  }

  // Blocks until `pred(events)` holds; false after 10 s of real time.
  bool wait(const std::function<bool(const std::vector<E>&)>& pred) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 10s, [&] { return pred(events_); });
  }
  std::vector<E> events() {
    std::lock_guard lock(mutex_);
    return events_;
  }
  void clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<E> events_;
  SignalBus::Subscription sub_;
};

class QtegraSystem : public ::testing::Test {
 protected:
  void SetUp() override {
    model_->clock = &clock_;
    model_->hv = 4500.0;
    model_->intensities = {{"H2", 10.0}, {"H1", 100.0}, {"AX", 20.0}, {"L1", 30.0}, {"L2", 40.0}, {"CDD", 500.0}};
  }

  void TearDown() override {
    link_->release();  // a failed test must not leave the pump at the gate
    pump_.reset();
    spec_.reset();
  }

  // Assembles `data` with every transport replaced by the Qtegra sim wire,
  // then starts the pump.
  void assemble(cfg::SpectrometerData data) {
    AssemblerOptions options;
    options.make_transport = [this](const cfg::TransportConfig& tc,
                                    const SpectrometerContext& ctx) -> Result<std::unique_ptr<Transport>> {
      TransportOptions to;
      to.name = tc.name;
      to.timeout = std::chrono::milliseconds(tc.timeout_ms);
      to.clock = &ctx.clock;
      return std::make_unique<LinkTransport>(SimTransport::hooked(qtegra_sim_hook(model_), to), link_);
    };
    auto spec = SpectrometerAssembler::assemble(std::move(data), SpectrometerContext{clock_, scheduler_, bus_},
                                                std::move(options));
    ASSERT_TRUE(spec.has_value()) << to_string(spec.error());
    spec_ = std::move(*spec);
    pump_ = std::make_unique<Pump>(clock_, scheduler_);
  }

  void assemble_example() {
    auto data = cfg::load_spectrometer(kQtegra);
    ASSERT_TRUE(data.has_value()) << to_string(data.error());
    assemble(std::move(*data));
  }

  std::vector<std::string> commands() {
    std::lock_guard lock(model_->mutex);
    return model_->commands;
  }
  void clear_commands() {
    std::lock_guard lock(model_->mutex);
    model_->commands.clear();
  }

  ManualClock clock_{TimePoint{} + 1000s};
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_, Scheduler::Options{0}};
  std::shared_ptr<QtegraSimModel> model_ = std::make_shared<QtegraSimModel>();
  std::shared_ptr<Link> link_ = std::make_shared<Link>();
  Collector<IntensityReading> readings_{bus_};
  Collector<ScanStatus> statuses_{bus_};
  std::unique_ptr<Spectrometer> spec_;
  std::unique_ptr<Pump> pump_;
};

TEST(QtegraExampleConfig, ExampleConfigLoadsAndValidates) {
  // load_spectrometer runs every assembler rule and opens nothing.
  auto data = cfg::load_spectrometer(kQtegra);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  const auto& c = data->config;
  ASSERT_EQ(c.transports.count("qtegra"), 1U);
  const auto& t = c.transports.at("qtegra");
  EXPECT_EQ(t.kind, cfg::TransportKind::Tcp);
  EXPECT_EQ(t.host, "192.168.0.10");
  EXPECT_EQ(t.tcp_port, 1069);
  EXPECT_EQ(t.timeout_ms, 2000);
  ASSERT_EQ(c.drivers.count("qtegra"), 1U);
  EXPECT_EQ(c.drivers.at("qtegra").kind, "thermo_qtegra");
  EXPECT_EQ(c.drivers.at("qtegra").transport, "qtegra");
  // The limits the driver enforces are shown, and agree with [magnet].limits.
  ASSERT_TRUE(c.magnet.limits.has_value());
  EXPECT_EQ(c.drivers.at("qtegra").options["limit_min"].value<double>(), c.magnet.limits->min);
  EXPECT_EQ(c.drivers.at("qtegra").options["limit_max"].value<double>(), c.magnet.limits->max);
  EXPECT_EQ(c.magnet.field_table, "argon");

  // Same detectors as the sim-integrated example, on this driver's channels.
  auto sim = cfg::load_spectrometer(kSimIntegrated);
  ASSERT_TRUE(sim.has_value()) << to_string(sim.error());
  ASSERT_EQ(c.detectors.size(), sim->config.detectors.size());
  for (std::size_t i = 0; i < c.detectors.size(); ++i) {
    EXPECT_EQ(c.detectors[i].name, sim->config.detectors[i].name);
    EXPECT_EQ(c.detectors[i].channel, "qtegra:" + c.detectors[i].name);
  }
}

TEST(QtegraExampleConfig, ExampleConfigIsNotSimulated) {
  auto data = cfg::load_spectrometer(kQtegra);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  EXPECT_FALSE(is_simulated(*data));

  // So --sim (require_sim) refuses it before anything is assembled or opened.
  ManualClock clock;
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
  auto spec = load_spectrometer_for_app(std::move(*data), SpectrometerContext{clock, scheduler, bus},
                                        SpectrometerBringup{.require_sim = true});
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().kind, ErrorKind::Config);
}

TEST_F(QtegraSystem, ConfigParityThroughSpectrometer) {
  // The sim-integrated example with only its driver swapped: same bindings,
  // detectors and tables, now on the real driver over the Qtegra sim wire.
  auto data = cfg::load_spectrometer(kSimIntegrated);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  auto& driver = data->config.drivers.at("sim");
  driver.kind = "thermo_qtegra";
  driver.options.erase("move_time_ms");  // a sim_integrated key the Qtegra schema does not declare
  assemble(std::move(*data));
  ASSERT_NE(spec_, nullptr);

  auto pos = spec_->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(pos.has_value()) << to_string(pos.error());
  EXPECT_GT(pos->native, 0.0);
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_NEAR(model_->dac, pos->native, 1e-6);
  }
  auto native = spec_->magnet_native();
  ASSERT_TRUE(native.has_value()) << to_string(native.error());
  EXPECT_NEAR(*native, pos->native, 1e-6);

  ASSERT_TRUE(spec_->set_hv(4400.0).has_value());
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_DOUBLE_EQ(model_->hv, 4400.0);
  }
  auto hv = spec_->read_hv();
  ASSERT_TRUE(hv.has_value()) << to_string(hv.error());
  EXPECT_DOUBLE_EQ(*hv, 4400.0);

  // The config asks for 1 s; Qtegra's nearest legal period is 1.048576 s.
  auto readings = spec_->acquire(3);
  ASSERT_TRUE(readings.has_value()) << to_string(readings.error());
  ASSERT_EQ(readings->size(), 3U);
  for (const auto& r : *readings) {
    EXPECT_NEAR(seconds(r.integration), 1.048576, 1e-9);
    ASSERT_EQ(r.values.size(), spec_->config().detectors.size());
    for (const auto& [det, value] : r.values) EXPECT_TRUE(value.has_value()) << det;
    EXPECT_DOUBLE_EQ(r.values.at("H1")->mean, 100.0);
    EXPECT_DOUBLE_EQ(r.values.at("AX")->mean, 20.0);
  }
}

TEST_F(QtegraSystem, MoveProtocolProtectsAndBlanks) {
  assemble_example();
  ASSERT_NE(spec_, nullptr);
  clear_commands();

  // 0 -> 5 V is above beam_blank_threshold (0.5): the CDD is protected and
  // the beam blanked for the move, then both undone in reverse order.
  auto moved = spec_->move_native(5.0);
  ASSERT_TRUE(moved.has_value()) << to_string(moved.error());
  EXPECT_TRUE(moved->blanked);
  EXPECT_EQ(moved->protected_channels, (std::vector<ChannelId>{"CDD"}));

  std::vector<std::string> sequence;
  for (const auto& c : commands()) {
    if (c.starts_with("SetMagnetDAC ")) {
      sequence.emplace_back("SetMagnetDAC");
    } else if (c.starts_with("ProtectDetector") || c.starts_with("BlankBeam")) {
      sequence.push_back(c);
    }
  }
  EXPECT_EQ(sequence, (std::vector<std::string>{"ProtectDetector CDD,On", "BlankBeam True", "SetMagnetDAC",
                                                "BlankBeam False", "ProtectDetector CDD,Off"}));
  std::lock_guard lock(model_->mutex);
  EXPECT_DOUBLE_EQ(model_->dac, 5.0);
  EXPECT_FALSE(model_->blank);
  EXPECT_FALSE(model_->protect.at("CDD"));
}

TEST_F(QtegraSystem, ContinuousScanDeliversAtSnappedPeriod) {
  assemble_example();
  ASSERT_NE(spec_, nullptr);
  ScanService service(*spec_, bus_, clock_);

  ASSERT_TRUE(service.start(1s).has_value());
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return rs.size() >= 4; }));
  const auto first = readings_.events();
  for (const auto& e : first) {
    EXPECT_NEAR(seconds(e.reading.integration), 1.048576, 1e-9);
    EXPECT_DOUBLE_EQ(e.reading.values.at("H1")->mean, 100.0);
  }
  expect_cadence(first, 1.048576);
  EXPECT_NEAR(seconds(service.integration()), 1.048576, 1e-9);

  ASSERT_TRUE(service.set_integration(500ms).has_value());
  // set_integration returns with the old run fully stopped, so everything
  // from here on belongs to the new one.
  readings_.clear();
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return rs.size() >= 4; }));
  const auto second = readings_.events();
  for (const auto& e : second) EXPECT_NEAR(seconds(e.reading.integration), 0.524288, 1e-9);
  expect_cadence(second, 0.524288);
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_DOUBLE_EQ(model_->integration_s, 0.524288);
  }
  ASSERT_TRUE(statuses_.wait([](const auto& ss) {
    return !ss.empty() && ss.back().running && std::abs(seconds(ss.back().integration) - 0.524288) < 1e-9;
  }));
  EXPECT_TRUE(service.status().error.empty()) << service.status().error;
}

// configure() never overlaps a next() of the run it replaces: with a GetData
// held on the wire, set_integration neither returns nor writes
// SetIntegrationTime until that read has finished.
TEST_F(QtegraSystem, SetIntegrationWaitsForAReadInFlight) {
  assemble_example();
  ASSERT_NE(spec_, nullptr);
  ScanService service(*spec_, bus_, clock_);
  ASSERT_TRUE(service.start(1s).has_value());
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return !rs.empty(); }));

  link_->hold_get_data();
  ASSERT_TRUE(link_->wait_held());  // the pump thread is inside next(), at the gate
  clear_commands();
  auto changed = std::async(std::launch::async, [&] { return service.set_integration(500ms); });
  // Only EXPECTs until the gate is open: `changed` cannot finish before then.
  // The bounded real-time wait is what gives an overlapping configure() the
  // chance to show itself.
  EXPECT_EQ(changed.wait_for(100ms), std::future_status::timeout);
  EXPECT_TRUE(commands().empty());

  link_->release();
  auto result = changed.get();
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  const auto log = commands();
  ASSERT_GE(log.size(), 2U);
  EXPECT_EQ(log[0], "GetData");
  EXPECT_TRUE(log[1].starts_with("SetIntegrationTime ")) << log[1];
}

// A dropped link shows as an error on the scan status. The engine keeps
// polling, so readings resume by themselves once the link is back; the status
// keeps the error until Restart (ScanService::start) clears it.
TEST_F(QtegraSystem, LinkDropDuringScanSurfacesErrorResumesAndRestartClearsIt) {
  assemble_example();
  ASSERT_NE(spec_, nullptr);
  ScanService service(*spec_, bus_, clock_);
  ASSERT_TRUE(service.start(1s).has_value());
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return !rs.empty(); }));

  // The connection drops and stays down: the driver's one reconnect attempt
  // (reopen, then the connect step) fails too, so next() returns the error.
  const int opens = link_->opens;
  link_->down = true;
  ASSERT_TRUE(statuses_.wait([](const auto& ss) { return !ss.empty() && !ss.back().error.empty(); }));
  EXPECT_TRUE(statuses_.events().back().running);
  EXPECT_GT(link_->opens, opens);

  // The link comes back: readings resume with no Restart, error still shown.
  const TimePoint back = clock_.now();
  link_->down = false;
  auto after = [&](TimePoint t) {
    return [&, t](const std::vector<IntensityReading>& rs) {
      return std::ranges::count_if(rs, [&](const auto& e) { return e.reading.ts > t; }) >= 2;
    };
  };
  ASSERT_TRUE(readings_.wait(after(back)));
  EXPECT_TRUE(service.running());
  EXPECT_FALSE(service.status().error.empty());

  // Restart clears it, and readings carry on.
  const TimePoint restarted = clock_.now();
  ASSERT_TRUE(service.start(1s).has_value());
  EXPECT_TRUE(service.status().error.empty()) << service.status().error;
  ASSERT_TRUE(readings_.wait(after(restarted)));
  EXPECT_NEAR(seconds(readings_.events().back().reading.integration), 1.048576, 1e-9);
  EXPECT_TRUE(service.running());
  EXPECT_TRUE(service.status().error.empty()) << service.status().error;
}

// Two layers hold a magnet move inside limits. The facade
// (Spectrometer::move_native / position) refuses a native value outside the
// positioner's own limits (the driver's limit_min/limit_max) or outside
// [magnet].limits, whichever bound is stricter, before it reads or writes
// anything. The driver's set() checks its own limits again
// (Qtegra.SetOutsideLimitsIsConfigAndWritesNothing). Nothing rejects a
// disagreement between the two at load time.

// [magnet].limits 0..20 V, driver 0..8 V: the driver's bound is the stricter.
TEST_F(QtegraSystem, MagnetLimitsDisagreeWithDriverLimits) {
  auto data = cfg::load_spectrometer(kQtegra);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  data->config.magnet.limits = cfg::Limits{0.0, 20.0};
  data->config.drivers.at("qtegra").options.insert_or_assign("limit_max", 8.0);
  assemble(std::move(*data));
  ASSERT_NE(spec_, nullptr);
  clear_commands();

  auto refused = spec_->move_native(9.0);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  EXPECT_TRUE(commands().empty());
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_DOUBLE_EQ(model_->dac, 0.0);
  }

  // Inside both, the same config moves.
  auto moved = spec_->move_native(7.0);
  ASSERT_TRUE(moved.has_value()) << to_string(moved.error());
  std::lock_guard lock(model_->mutex);
  EXPECT_DOUBLE_EQ(model_->dac, 7.0);
}

// The mirror: [magnet].limits 0..6 V, driver 0..10 V. The driver would accept
// 7 V; the facade refuses it on the config's bound.
TEST_F(QtegraSystem, MagnetLimitsNarrowerThanDriverLimitsAreEnforced) {
  auto data = cfg::load_spectrometer(kQtegra);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  data->config.magnet.limits = cfg::Limits{0.0, 6.0};
  data->config.drivers.at("qtegra").options.insert_or_assign("limit_max", 10.0);
  assemble(std::move(*data));
  ASSERT_NE(spec_, nullptr);
  clear_commands();

  for (const auto& target : {PositionTarget{NativeUnits{7.0}, ""}, PositionTarget{NativeUnits{-0.5}, ""}}) {
    auto refused = spec_->position(target);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  }
  auto refused = spec_->move_native(7.0);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  EXPECT_TRUE(commands().empty());
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_DOUBLE_EQ(model_->dac, 0.0);
  }

  auto moved = spec_->move_native(5.0);
  ASSERT_TRUE(moved.has_value()) << to_string(moved.error());
  std::lock_guard lock(model_->mutex);
  EXPECT_DOUBLE_EQ(model_->dac, 5.0);
}

}  // namespace
