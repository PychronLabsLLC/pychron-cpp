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
  std::atomic<int> in_flight{0};  // exchanges between call and return
  std::atomic<int> overlaps{0};   // exchanges begun while another was in flight
};

// Owns the simulated wire. While the link is down an exchange fails before
// reaching it; reopening still succeeds.
class LinkTransport final : public Transport {
 public:
  LinkTransport(std::unique_ptr<SimTransport> inner, std::shared_ptr<Link> link)
      : inner_(std::move(inner)), link_(std::move(link)) {}

  const std::string& name() const override { return inner_->name(); }
  Result<void> open() override { return inner_->open(); }
  void close() override { inner_->close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override {
    if (link_->down) return fail(ErrorKind::Io, "connection reset", inner_->name());
    if (link_->in_flight.fetch_add(1) > 0) ++link_->overlaps;
    auto reply = inner_->exchange(std::move(tx), std::move(rs), timeout);
    --link_->in_flight;
    return reply;
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
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return rs.size() >= 3; }));
  const auto first = readings_.events();
  for (const auto& e : first) {
    EXPECT_NEAR(seconds(e.reading.integration), 1.048576, 1e-9);
    EXPECT_DOUBLE_EQ(e.reading.values.at("H1")->mean, 100.0);
  }
  EXPECT_NEAR(seconds(service.integration()), 1.048576, 1e-9);

  ASSERT_TRUE(service.set_integration(500ms).has_value());
  // set_integration returns with the old run fully stopped, so everything
  // from here on belongs to the new one.
  readings_.clear();
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return rs.size() >= 3; }));
  for (const auto& e : readings_.events()) EXPECT_NEAR(seconds(e.reading.integration), 0.524288, 1e-9);
  {
    std::lock_guard lock(model_->mutex);
    EXPECT_DOUBLE_EQ(model_->integration_s, 0.524288);
  }
  ASSERT_TRUE(statuses_.wait([](const auto& ss) {
    return !ss.empty() && ss.back().running && std::abs(seconds(ss.back().integration) - 0.524288) < 1e-9;
  }));
  EXPECT_TRUE(service.status().error.empty()) << service.status().error;

  service.stop();
  // configure() (this thread) and next() (the pump thread) never had the wire
  // at the same time.
  EXPECT_EQ(link_->overlaps, 0);
}

TEST_F(QtegraSystem, ReconnectDuringScanSurfacesErrorAndRestartRecovers) {
  assemble_example();
  ASSERT_NE(spec_, nullptr);
  ScanService service(*spec_, bus_, clock_);
  ASSERT_TRUE(service.start(1s).has_value());
  ASSERT_TRUE(readings_.wait([](const auto& rs) { return !rs.empty(); }));

  // The connection drops and stays down: the driver's one reconnect attempt
  // fails too, so next() returns the error and the scan status carries it.
  link_->down = true;
  ASSERT_TRUE(statuses_.wait([](const auto& ss) { return !ss.empty() && !ss.back().error.empty(); }));
  EXPECT_TRUE(statuses_.events().back().running);
  EXPECT_FALSE(service.status().error.empty());

  // The link comes back; Restart is ScanService::start again.
  link_->down = false;
  const TimePoint restarted = clock_.now();
  ASSERT_TRUE(service.start(1s).has_value());
  ASSERT_TRUE(readings_.wait([&](const auto& rs) {
    return std::ranges::count_if(rs, [&](const auto& e) { return e.reading.ts > restarted; }) >= 2;
  }));
  EXPECT_NEAR(seconds(readings_.events().back().reading.integration), 1.048576, 1e-9);
  EXPECT_TRUE(service.running());
  EXPECT_TRUE(service.status().error.empty()) << service.status().error;
}

TEST_F(QtegraSystem, MagnetLimitsDisagreeWithDriverLimits) {
  // [magnet].limits says 0..20 V; the driver is limited to 0..8 V. Nothing
  // rejects the disagreement at load time. A move to 9 V is refused by the
  // facade (Spectrometer::move_native), which checks the positioner's own
  // limits(), that is the driver's limit_min/limit_max and not
  // [magnet].limits, before it reads or writes anything. The driver's set()
  // makes the same check again (Qtegra.SetOutsideLimitsIsConfigAndWritesNothing),
  // so it is the driver's limits that guard the wire.
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

  // Inside the driver's limits the same config moves.
  auto moved = spec_->move_native(7.0);
  ASSERT_TRUE(moved.has_value()) << to_string(moved.error());
  std::lock_guard lock(model_->mutex);
  EXPECT_DOUBLE_EQ(model_->dac, 7.0);
}

}  // namespace
