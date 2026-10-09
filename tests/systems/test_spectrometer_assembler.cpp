#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "pychron/devices/connectable.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "spectrometer_fakes.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kDir(PYCHRON_EXAMPLE_CONFIGS_DIR);

cfg::SpectrometerData load(const char* file) {
  auto d = cfg::load_spectrometer(kDir / file);
  EXPECT_TRUE(d.has_value()) << (d ? "" : d.error().what);
  return std::move(*d);
}

// A device that plays only the positioner role.
struct PositionerOnly : Device, FakePositioner {
  explicit PositionerOnly(CallLog& call_log) : Device("magnet"), FakePositioner(call_log) {}
};

struct AcquirerOnly : Device, FakeAcquirer {
  explicit AcquirerOnly(std::vector<ChannelId> channel_ids) : Device("acq"), FakeAcquirer(std::move(channel_ids)) {}
};

struct Env {
  ManualClock clock;
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
  SpectrometerContext ctx() { return SpectrometerContext{clock, scheduler, bus}; }
  ~Env() { sim::BeamModelRegistry::global().clear(); }
};

}  // namespace

TEST(SpectrometerAssembler, BindReportsEveryProblem) {
  auto data = load("spectrometer.sim-legacy.toml");
  CallLog log;
  PositionerOnly magnet(log);
  magnet.axis = IMassPositioner::Axis::Field;  // config says dac
  AcquirerOnly faradays({"AX", "H1"});         // L1 missing
  PositionerOnly not_an_acquirer(log);
  // hv_supply not built at all
  auto roles = SpectrometerAssembler::bind(
      data.config, {{"magnet_dac", &magnet}, {"faradays", &faradays}, {"multiplier", &not_an_acquirer}});
  ASSERT_FALSE(roles.has_value());
  const std::string& what = roles.error().what;
  EXPECT_EQ(roles.error().kind, ErrorKind::Config);
  EXPECT_NE(what.find("natively field"), std::string::npos) << what;
  EXPECT_NE(what.find("no channel 'L1'"), std::string::npos) << what;
  EXPECT_NE(what.find("acquirer: driver 'multiplier' does not implement it"), std::string::npos) << what;
  EXPECT_NE(what.find("source: driver 'hv_supply' was not built"), std::string::npos) << what;
}

TEST(SpectrometerAssembler, BindSucceedsForMatchingDevices) {
  auto data = load("spectrometer.sim-legacy.toml");
  CallLog log;
  PositionerOnly magnet(log);
  AcquirerOnly faradays({"AX", "H1", "L1"});
  AcquirerOnly counter({"EM"});
  struct Hv : Device, FakeSource {
    Hv() : Device("hv") {}
  } hv;
  auto roles = SpectrometerAssembler::bind(
      data.config, {{"magnet_dac", &magnet}, {"faradays", &faradays}, {"multiplier", &counter}, {"hv_supply", &hv}});
  ASSERT_TRUE(roles.has_value()) << roles.error().what;
  EXPECT_EQ(roles->positioner, &magnet);
  EXPECT_EQ(roles->acquirers.size(), 2U);
  EXPECT_EQ(roles->detector_control, nullptr);
  EXPECT_EQ(roles->beam_blank, nullptr);
}

TEST(SpectrometerAssembler, ConfigRulesRunBeforeBuilding) {
  auto data = load("spectrometer.sim-integrated.toml");
  data.config.magnet.native_axis = cfg::Axis::Mass;  // HV correction enabled -> rule violation
  Env env;
  bool built_anything = false;
  AssemblerOptions options;
  options.make_transport = [&](const cfg::TransportConfig&, const SpectrometerContext&)
      -> Result<std::unique_ptr<Transport>> {
    built_anything = true;
    return fail(ErrorKind::Config, "unreachable");
  };
  auto spec = SpectrometerAssembler::assemble(std::move(data), env.ctx(), options);
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().kind, ErrorKind::Config);
  EXPECT_FALSE(built_anything);
}

TEST(SpectrometerAssembler, DriverFailuresAreCollected) {
  auto data = load("spectrometer.sim-legacy.toml");
  Env env;
  AssemblerOptions options;
  options.make_driver = [](const cfg::DriverConfig& dc, Transport&, const SpectrometerContext&)
      -> Result<std::unique_ptr<Device>> { return fail(ErrorKind::Io, "no " + dc.name); };
  auto spec = SpectrometerAssembler::assemble(std::move(data), env.ctx(), options);
  ASSERT_FALSE(spec.has_value());
  for (const char* d : {"magnet_dac", "faradays", "multiplier", "hv_supply"}) {
    EXPECT_NE(spec.error().what.find(std::string("driver '") + d + "'"), std::string::npos) << spec.error().what;
  }
}

TEST(SpectrometerAssembler, AssemblesBothSimConfigsFromRegistry) {
  for (const char* file : {"spectrometer.sim-integrated.toml", "spectrometer.sim-legacy.toml"}) {
    Env env;
    auto spec = SpectrometerAssembler::load(kDir / file, env.ctx());
    ASSERT_TRUE(spec.has_value()) << file << ": " << spec.error().what;
    EXPECT_EQ((*spec)->native_axis(), IMassPositioner::Axis::Dac);
    EXPECT_EQ((*spec)->reference_detector(), "H1");
    EXPECT_EQ((*spec)->active_table(), (*spec)->config().magnet.field_table);
    EXPECT_EQ((*spec)->detectors().size(), (*spec)->config().detectors.size());
  }
}

TEST(SpectrometerAssembler, TraceKeyWritesTraceFile) {
  auto data = load("spectrometer.sim-legacy.toml");
  const auto dir = std::filesystem::temp_directory_path() / "pychron_spec_trace_test" / "nested";
  std::filesystem::remove_all(dir.parent_path());
  const std::string name = data.config.transports.begin()->first;
  data.config.transports.begin()->second.trace = true;
  Env env;
  AssemblerOptions options;
  options.spectrometer.trace_dir = dir;
  auto spec = SpectrometerAssembler::assemble(std::move(data), env.ctx(), options);
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  EXPECT_TRUE(std::filesystem::exists(dir / (name + ".trace")));
  spec->reset();
  std::filesystem::remove_all(dir.parent_path());
}

TEST(SpectrometerAssembler, TraceDirCreationFailureIsIoError) {
  auto data = load("spectrometer.sim-legacy.toml");
  const auto blocker = std::filesystem::temp_directory_path() / "pychron_spec_trace_blocker";
  std::filesystem::remove_all(blocker);
  { std::ofstream(blocker) << "x"; }
  data.config.transports.begin()->second.trace = true;
  Env env;
  AssemblerOptions options;
  options.spectrometer.trace_dir = blocker / "traces";  // parent is a regular file
  auto spec = SpectrometerAssembler::assemble(std::move(data), env.ctx(), options);
  std::filesystem::remove(blocker);
  ASSERT_FALSE(spec.has_value());
  EXPECT_NE(spec.error().what.find("cannot create trace directory"), std::string::npos) << spec.error().what;
}

// ---- opening and connecting ---------------------------------------------------

namespace {

using Events = std::vector<std::string>;

// Forwards to a SimTransport and records open/close in `events`.
class ProbeTransport final : public Transport {
 public:
  ProbeTransport(std::string name, Events& events)
      : name_(std::move(name)), events_(events), inner_(SimTransport::hooked([](const Bytes&) { return Bytes{}; })) {}

  const std::string& name() const override { return name_; }
  Result<void> open() override {
    auto r = inner_->open();
    events_.push_back((r ? "open:" : "open-failed:") + name_);
    return r;
  }
  void close() override {
    if (is_open()) events_.push_back("close:" + name_);
    inner_->close();
  }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration t = kDefaultTimeout) override {
    return inner_->exchange(std::move(tx), std::move(rs), t);
  }
  Result<void> write(Bytes tx) override { return inner_->write(std::move(tx)); }
  Result<Bytes> read(ReadSpec rs, Duration t = kDefaultTimeout) override { return inner_->read(std::move(rs), t); }
  Result<void> transaction(std::function<Result<void>()> body) override {
    return inner_->transaction(std::move(body));
  }
  Health health() const override { return inner_->health(); }

  bool is_open() const { return health().state != HealthState::Down; }
  SimTransport& sim() { return *inner_; }

 private:
  std::string name_;
  Events& events_;
  std::unique_ptr<SimTransport> inner_;
};

// Role fake that also connects, recording whether its transport was open.
template <class Role>
struct ConnectingDriver : Device, Role, IConnectable {
  template <class... A>
  ConnectingDriver(std::string name, ProbeTransport& transport, Events& events, A&&... a)
      : Device(name), Role(std::forward<A>(a)...), transport_(transport), events_(events) {}
  Result<void> connect() override {
    events_.push_back(std::string(transport_.is_open() ? "connect:" : "connect-closed:") + name());
    if (fail_connect) return fail(ErrorKind::Timeout, "no answer");
    return {};
  }
  ProbeTransport& transport_;
  Events& events_;
  bool fail_connect = false;
};

struct Rig {
  Env env;
  Events events;
  CallLog log;
  std::map<std::string, ProbeTransport*> probes;  // valid until the spectrometer is gone
  std::string fail_connect_driver;
  std::string fail_open_transport;
  bool connecting = true;

  AssemblerOptions options() {
    AssemblerOptions o;
    o.make_transport = [this](const cfg::TransportConfig& tc,
                              const SpectrometerContext&) -> Result<std::unique_ptr<Transport>> {
      auto t = std::make_unique<ProbeTransport>(tc.name, events);
      if (tc.name == fail_open_transport) t->sim().fail_open_next();
      probes[tc.name] = t.get();
      return std::unique_ptr<Transport>(std::move(t));
    };
    o.make_driver = [this](const cfg::DriverConfig& dc, Transport& t,
                           const SpectrometerContext&) -> Result<std::unique_ptr<Device>> {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the test made it
      auto& probe = static_cast<ProbeTransport&>(t);
      std::unique_ptr<Device> d;
      auto make = [&](auto driver) {
        driver->fail_connect = dc.name == fail_connect_driver;
        d = std::move(driver);
      };
      if (dc.name == "magnet_dac") {
        make(std::make_unique<ConnectingDriver<FakePositioner>>(dc.name, probe, events, log));
      } else if (dc.name == "faradays") {
        make(std::make_unique<ConnectingDriver<FakeAcquirer>>(dc.name, probe, events,
                                                              std::vector<ChannelId>{"AX", "H1", "L1"}));
      } else if (dc.name == "multiplier") {
        make(std::make_unique<ConnectingDriver<FakeAcquirer>>(dc.name, probe, events,
                                                              std::vector<ChannelId>{"EM"}));
      } else {
        make(std::make_unique<ConnectingDriver<FakeSource>>(dc.name, probe, events));
      }
      return d;
    };
    // NOLINTNEXTLINE(clang-analyzer-core.StackAddressEscape): returned by value; nothing of this frame is captured
    return o;
  }
};

}  // namespace

TEST(SpectrometerAssembler, AssembleOpensEveryTransport) {
  Rig rig;
  auto spec = SpectrometerAssembler::assemble(load("spectrometer.sim-legacy.toml"), rig.env.ctx(), rig.options());
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  ASSERT_EQ(rig.probes.size(), 4U);
  for (const auto& [name, probe] : rig.probes) {
    EXPECT_EQ(probe->health().state, HealthState::Connected) << name;
  }
  // Config (map) order.
  EXPECT_EQ((Events{rig.events.begin(), rig.events.begin() + 4}),
            (Events{"open:adc", "open:counter", "open:dac", "open:hv"}));
}

TEST(SpectrometerAssembler, OpenFailureClosesOpenedTransportsAndReturnsError) {
  Rig rig;
  rig.fail_open_transport = "counter";  // second in config order
  auto spec = SpectrometerAssembler::assemble(load("spectrometer.sim-legacy.toml"), rig.env.ctx(), rig.options());
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().kind, ErrorKind::Io);
  EXPECT_EQ(spec.error().device, "counter");
  EXPECT_EQ(rig.events, (Events{"open:adc", "open-failed:counter", "close:adc"}));
}

TEST(SpectrometerAssembler, ConnectCalledAfterOpenInDriverOrder) {
  Rig rig;
  auto spec = SpectrometerAssembler::assemble(load("spectrometer.sim-legacy.toml"), rig.env.ctx(), rig.options());
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  // Every transport opens before the first connect; drivers connect in config order,
  // each seeing its transport open.
  EXPECT_EQ(rig.events, (Events{"open:adc", "open:counter", "open:dac", "open:hv", "connect:faradays",
                                "connect:hv_supply", "connect:magnet_dac", "connect:multiplier"}));
}

TEST(SpectrometerAssembler, ConnectFailureClosesTransportsAndReturnsError) {
  Rig rig;
  rig.fail_connect_driver = "hv_supply";
  auto spec = SpectrometerAssembler::assemble(load("spectrometer.sim-legacy.toml"), rig.env.ctx(), rig.options());
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(spec.error().device, "hv_supply");
  const Events tail(rig.events.begin() + 4, rig.events.end());
  EXPECT_EQ(tail, (Events{"connect:faradays", "connect:hv_supply", "close:adc", "close:counter", "close:dac",
                          "close:hv"}));
}

TEST(SpectrometerAssembler, DestructorClosesTransports) {
  Rig rig;
  auto spec = SpectrometerAssembler::assemble(load("spectrometer.sim-legacy.toml"), rig.env.ctx(), rig.options());
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  const auto before = rig.events.size();
  spec->reset();
  const Events closed(rig.events.begin() + before, rig.events.end());
  EXPECT_EQ(closed, (Events{"close:adc", "close:counter", "close:dac", "close:hv"}));
}

TEST(SpectrometerAssembler, ExistingSimConfigsStillAssemble) {
  for (const char* file : {"spectrometer.sim-integrated.toml", "spectrometer.sim-legacy.toml"}) {
    Env env;
    auto spec = SpectrometerAssembler::load(kDir / file, env.ctx());
    ASSERT_TRUE(spec.has_value()) << file << ": " << spec.error().what;
  }
}
