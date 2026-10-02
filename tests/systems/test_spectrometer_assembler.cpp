#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "pychron/sim/spectrometer/beam_model.hpp"
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
  explicit PositionerOnly(CallLog& log) : Device("magnet"), FakePositioner(log) {}
};

struct AcquirerOnly : Device, FakeAcquirer {
  explicit AcquirerOnly(std::vector<ChannelId> chans) : Device("acq"), FakeAcquirer(std::move(chans)) {}
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
