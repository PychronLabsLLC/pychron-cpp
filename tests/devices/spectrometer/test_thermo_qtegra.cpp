#include "pychron/devices/spectrometer/thermo_qtegra.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>

#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

namespace {

// Forwards to a SimTransport; while `fail_exchanges` > 0 an exchange fails
// with Io before reaching the wire (a dropped connection).
class FlakyTransport final : public Transport {
 public:
  explicit FlakyTransport(Transport& inner) : inner_(inner) {}

  const std::string& name() const override { return inner_.name(); }
  Result<void> open() override {
    ++opens;
    return inner_.open();
  }
  void close() override {
    ++closes;
    inner_.close();
  }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override {
    if (fail_exchanges > 0) {
      --fail_exchanges;
      return fail(ErrorKind::Io, "connection reset", inner_.name());
    }
    return inner_.exchange(std::move(tx), std::move(rs), timeout);
  }
  Result<void> write(Bytes tx) override { return inner_.write(std::move(tx)); }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override { return inner_.read(std::move(rs), timeout); }
  Result<void> transaction(std::function<Result<void>()> body) override { return inner_.transaction(std::move(body)); }
  Health health() const override { return inner_.health(); }

  std::atomic<int> fail_exchanges{0};
  std::atomic<int> opens{0};
  std::atomic<int> closes{0};

 private:
  Transport& inner_;
};

std::vector<std::string> written_text(const SimTransport& sim) {
  std::vector<std::string> out;
  for (const auto& tx : sim.written()) out.push_back(to_string(tx));
  return out;
}

template <class T>
void expect_config(const Result<T>& r) {
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config) << to_string(r.error());
}

Result<std::unique_ptr<Device>> create_from(SimTransport& sim, std::string_view toml) {
  return DriverRegistry::global().create("thermo_qtegra", sim, table_of(toml), DriverContext{"argus"});
}

}  // namespace

// --- registry ------------------------------------------------------------------

TEST(Qtegra, CreatesFromTomlTable) {
  auto sim = open_scripted({step("GetMagnetDAC\n", "4.5\r\n")});
  auto dev = create_from(*sim, R"(
    roles = ["positioner", "source", "acquirer", "detector_control", "beam_blank"]
    channels = ["H1", "AX"]
    limit_min = 1.0
    limit_max = 9.0
    terminator = "lf"
    settle_periods = 3.0
  )");
  ASSERT_TRUE(dev) << to_string(dev.error());
  EXPECT_EQ((*dev)->name(), "argus");
  EXPECT_NE(dynamic_cast<IConnectable*>(dev->get()), nullptr);
  EXPECT_NE(dynamic_cast<IBeamSource*>(dev->get()), nullptr);
  EXPECT_NE(dynamic_cast<IDetectorControl*>(dev->get()), nullptr);
  EXPECT_NE(dynamic_cast<IBeamBlank*>(dev->get()), nullptr);
  auto* positioner = dynamic_cast<IMassPositioner*>(dev->get());
  ASSERT_NE(positioner, nullptr);
  EXPECT_EQ(positioner->limits(), (Limits{1.0, 9.0}));
  auto* acquirer = dynamic_cast<IIntensityAcquirer*>(dev->get());
  ASSERT_NE(acquirer, nullptr);
  EXPECT_EQ(acquirer->channels(), (std::vector<ChannelId>{"H1", "AX"}));
  EXPECT_TRUE(acquirer->integrates());
  EXPECT_DOUBLE_EQ(*positioner->read(), 4.5);
  expect_verified(*sim);
}

TEST(Qtegra, DefaultsWhenTableIsEmpty) {
  auto sim = open_scripted({});
  auto dev = create_from(*sim, "");
  ASSERT_TRUE(dev) << to_string(dev.error());
  EXPECT_EQ(dynamic_cast<IMassPositioner*>(dev->get())->limits(), (Limits{0.0, 10.0}));
  EXPECT_EQ(dynamic_cast<IIntensityAcquirer*>(dev->get())->channels(),
            (std::vector<ChannelId>{"H2", "H1", "AX", "L1", "L2", "CDD"}));
}

TEST(Qtegra, SchemaListsDeclaredKeys) {
  const DriverSchema* schema = DriverRegistry::global().schema("thermo_qtegra");
  ASSERT_NE(schema, nullptr);
  std::vector<std::string> names;
  for (const auto& key : schema->keys) {
    names.push_back(key.name);
    EXPECT_FALSE(key.required) << key.name;
  }
  EXPECT_EQ(names, (std::vector<std::string>{"roles", "channels", "limit_min", "limit_max", "terminator",
                                             "settle_periods"}));
}

TEST(Qtegra, UndeclaredKeyRejected) {
  auto sim = open_scripted({});
  expect_config(create_from(*sim, "host = \"10.0.0.1\""));
}

TEST(Qtegra, BadTerminatorRejected) {
  auto sim = open_scripted({});
  expect_config(create_from(*sim, "terminator = \"nul\""));
  EXPECT_TRUE(create_from(*sim, "terminator = \"crlf\""));
}

TEST(Qtegra, InvertedLimitsRejected) {
  auto sim = open_scripted({});
  expect_config(create_from(*sim, "limit_min = 6.0\nlimit_max = 5.0"));
  expect_config(create_from(*sim, "limit_min = 5.0\nlimit_max = 5.0"));
}

TEST(Qtegra, EmptyOrDuplicateChannelsRejected) {
  auto sim = open_scripted({});
  expect_config(create_from(*sim, "channels = []"));
  expect_config(create_from(*sim, "channels = [\"H1\", \"H1\"]"));
  expect_config(create_from(*sim, "channels = [\"\"]"));
}

TEST(Qtegra, ChannelNameTheCodecCannotEncodeRejected) {
  auto sim = open_scripted({});
  expect_config(create_from(*sim, "channels = [\"H1,AX\"]"));
  expect_config(create_from(*sim, "channels = [\"H1\\r\"]"));
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, NegativeSettlePeriodsRejected) {
  auto sim = open_scripted({});
  expect_config(create_from(*sim, "settle_periods = -1.0"));
}

// --- connect -------------------------------------------------------------------

TEST(Qtegra, ConnectSendsGetIntegrationTime) {
  auto sim = open_scripted({step("GetIntegrationTime\r", "1.048576\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto r = q.connect();
  ASSERT_TRUE(r) << to_string(r.error());
  expect_verified(*sim);
  EXPECT_EQ(q.health().state, DeviceState::Ok);
}

TEST(Qtegra, ConnectNonNumericReplyIsProtocol) {
  auto sim = open_scripted({step("GetIntegrationTime\r", "busy\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto r = q.connect();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "argus");
}

TEST(Qtegra, ConnectTimeoutIsReturned) {
  auto sim = open_scripted({SimStep{to_bytes("GetIntegrationTime\r"), to_bytes("1.048576\r\n"), 1s}});
  QtegraSpectrometer q("argus", *sim, {});
  auto r = q.connect();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(q.reconnects(), 0U);
}

// --- positioner ----------------------------------------------------------------

TEST(Qtegra, SetSendsSetMagnetDac) {
  auto sim = open_scripted({step("SetMagnetDAC 5.001\r", "OK\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto r = q.set(5.001);
  ASSERT_TRUE(r) << to_string(r.error());
  expect_verified(*sim);
}

// pychron ignores the replies to these five setters, so anything that is not
// an explicit ERROR is accepted.
TEST(Qtegra, SettersAcceptAnyNonErrorReply) {
  auto sim = open_scripted({step("SetMagnetDAC 2\r", "2\r\n"), step("BlankBeam True\r", "True\r\n"),
                            step("ProtectDetector CDD,On\r", "CDD\r\n"), step("SetDeflection H1,12.5\r", "12.5\r\n"),
                            step("SetGain AX,1.002\r", "done\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_TRUE(q.set(2.0));
  EXPECT_TRUE(q.blank(true));
  EXPECT_TRUE(q.protect("CDD", true));
  EXPECT_TRUE(q.set_deflection("H1", 12.5));
  EXPECT_TRUE(q.set_gain("AX", 1.002));
  expect_verified(*sim);
  EXPECT_EQ(q.health().state, DeviceState::Ok);
}

TEST(Qtegra, SettersTreatErrorReplyAsProtocol) {
  auto sim = open_scripted({step("SetMagnetDAC 2\r", "ERROR: magnet\r\n"), step("BlankBeam True\r", "ERROR: bad\r\n"),
                            step("ProtectDetector CDD,On\r", "ERROR: no CDD\r\n"),
                            step("SetDeflection H1,12.5\r", "ERROR\r\n"), step("SetGain AX,1.002\r", "ERROR: bad\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  for (const auto& r : {q.set(2.0), q.blank(true), q.protect("CDD", true), q.set_deflection("H1", 12.5),
                        q.set_gain("AX", 1.002)}) {
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    EXPECT_EQ(r.error().device, "argus");
  }
  expect_verified(*sim);
  EXPECT_EQ(q.reconnects(), 0U);
}

TEST(Qtegra, SetterTimeoutIsStillAnError) {
  auto sim = open_scripted({SimStep{to_bytes("BlankBeam True\r"), to_bytes("OK\r\n"), 1s}});
  QtegraSpectrometer q("argus", *sim, {});
  auto r = q.blank(true);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
}

TEST(Qtegra, SetOutsideLimitsIsConfigAndWritesNothing) {
  auto sim = open_scripted({});
  QtegraOptions options;
  options.limits = {1.0, 9.0};
  QtegraSpectrometer q("argus", *sim, options);
  const double inf = std::numeric_limits<double>::infinity();
  for (double v : {0.999, 9.001, std::numeric_limits<double>::quiet_NaN(), inf, -inf}) expect_config(q.set(v));
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, SetAtExactLimitsIsAccepted) {
  auto sim = open_scripted({step("SetMagnetDAC 1\r", "OK\r\n"), step("SetMagnetDAC 9\r", "OK\r\n")});
  QtegraOptions options;
  options.limits = {1.0, 9.0};
  QtegraSpectrometer q("argus", *sim, options);
  EXPECT_TRUE(q.set(1.0));
  EXPECT_TRUE(q.set(9.0));
  expect_verified(*sim);
}

TEST(Qtegra, ReadParsesGetMagnetDac) {
  auto sim = open_scripted({step("GetMagnetDAC\r", "4.5\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto v = q.read();
  ASSERT_TRUE(v) << to_string(v.error());
  EXPECT_DOUBLE_EQ(*v, 4.5);
  expect_verified(*sim);
}

TEST(Qtegra, MovingUsesBoolVocabulary) {
  auto sim = open_scripted({step("GetMagnetMoving\r", "True\r\n"), step("GetMagnetMoving\r", "false\r\n"),
                            step("GetMagnetMoving\r", "1\r\n"), step("GetMagnetMoving\r", "maybe\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_TRUE(*q.moving());
  EXPECT_FALSE(*q.moving());
  EXPECT_TRUE(*q.moving());
  auto bad = q.moving();
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, ErrorKind::Protocol);
  expect_verified(*sim);
}

TEST(Qtegra, NativeAxisIsDac) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_EQ(q.native_axis(), IMassPositioner::Axis::Dac);
}

TEST(Qtegra, LimitsFromOptions) {
  auto sim = open_scripted({});
  QtegraOptions options;
  options.limits = {0.5, 8.0};
  QtegraSpectrometer q("argus", *sim, options);
  EXPECT_EQ(q.limits(), (Limits{0.5, 8.0}));
  EXPECT_EQ(QtegraSpectrometer("argus", *sim, {}).limits(), (Limits{0.0, 10.0}));
}

// --- beam blank ----------------------------------------------------------------

TEST(Qtegra, BlankSendsTrueAndFalse) {
  auto sim = open_scripted({step("BlankBeam True\r", "OK\r\n"), step("BlankBeam False\r", "OK\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_TRUE(q.blank(true));
  EXPECT_TRUE(q.blank(false));
  expect_verified(*sim);
}

// --- detector control ----------------------------------------------------------

TEST(Qtegra, CapsAreGainDeflectionProtect) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_EQ(q.caps(), DetectorCap::Gain | DetectorCap::Deflection | DetectorCap::Protect);
}

TEST(Qtegra, ProtectSendsMagnetMoveForm) {
  auto sim = open_scripted({step("ProtectDetector CDD,On\r", "OK\r\n"), step("ProtectDetector CDD,Off\r", "OK\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_TRUE(q.protect("CDD", true));
  EXPECT_TRUE(q.protect("CDD", false));
  expect_verified(*sim);
}

TEST(Qtegra, DeflectionRoundTrip) {
  auto sim = open_scripted({step("SetDeflection H1,12.5\r", "OK\r\n"), step("GetDeflection H1\r", "12.5\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  ASSERT_TRUE(q.set_deflection("H1", 12.5));
  auto v = q.read_deflection("H1");
  ASSERT_TRUE(v) << to_string(v.error());
  EXPECT_DOUBLE_EQ(*v, 12.5);
  expect_verified(*sim);
}

TEST(Qtegra, GainRoundTrip) {
  auto sim = open_scripted({step("SetGain AX,1.002\r", "OK\r\n"), step("GetGain AX\r", "1.002\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  ASSERT_TRUE(q.set_gain("AX", 1.002));
  auto v = q.read_gain("AX");
  ASSERT_TRUE(v) << to_string(v.error());
  EXPECT_DOUBLE_EQ(*v, 1.002);
  expect_verified(*sim);
}

TEST(Qtegra, UnknownChannelIsConfigAndWritesNothing) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  expect_config(q.protect("H9", true));
  expect_config(q.set_deflection("H9", 1.0));
  expect_config(q.read_deflection("H9"));
  expect_config(q.set_gain("H9", 1.0));
  expect_config(q.read_gain("H9"));
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, NonFiniteDetectorValueIsConfigAndWritesNothing) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  expect_config(q.set_deflection("H1", std::numeric_limits<double>::infinity()));
  expect_config(q.set_gain("H1", std::numeric_limits<double>::quiet_NaN()));
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, SetCddVoltageIsUnsupportedConfig) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  expect_config(q.set_cdd_voltage("CDD", 1800.0));
  EXPECT_TRUE(sim->written().empty());
}

// --- terminator ----------------------------------------------------------------

TEST(Qtegra, LfTerminatorUsedWhenConfigured) {
  auto sim = open_scripted({step("GetMagnetDAC\n", "4.5\r\n"), step("BlankBeam True\n", "OK\r\n")});
  QtegraOptions options;
  options.terminator = codec::qtegra::Terminator::LF;
  QtegraSpectrometer q("argus", *sim, options);
  EXPECT_TRUE(q.read());
  EXPECT_TRUE(q.blank(true));
  expect_verified(*sim);
}

// --- errors --------------------------------------------------------------------

TEST(Qtegra, ProtocolErrorCarriesDeviceName) {
  auto sim = open_scripted({step("GetMagnetDAC\r", "volts\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto v = q.read();
  ASSERT_FALSE(v);
  EXPECT_EQ(v.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(v.error().device, "argus");
  EXPECT_EQ(q.health().state, DeviceState::Degraded);
}

TEST(Qtegra, ErrorReplyIsProtocol) {
  auto sim = open_scripted({step("GetMagnetDAC\r", "ERROR: bad\r\n"), step("BlankBeam True\r", "ERROR: bad\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto v = q.read();
  ASSERT_FALSE(v);
  EXPECT_EQ(v.error().kind, ErrorKind::Protocol);
  auto b = q.blank(true);
  ASSERT_FALSE(b);
  EXPECT_EQ(b.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(q.reconnects(), 0U);
}

// --- source --------------------------------------------------------------------

TEST(Qtegra, SetHvExpectsOk) {
  auto sim = open_scripted({step("SetHV 4500\r", "OK\r\n"), step("SetHV 9900.5\r", "ok\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_TRUE(q.set_hv(4500.0));
  EXPECT_TRUE(q.set_hv(9900.5));
  expect_verified(*sim);
}

TEST(Qtegra, SetHvNonOkIsProtocol) {
  auto sim = open_scripted({step("SetHV 4500\r", "4500\r\n"), step("SetHV 4500\r", "ERROR: interlock\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  for (int i = 0; i < 2; ++i) {
    auto r = q.set_hv(4500.0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    EXPECT_EQ(r.error().device, "argus");
  }
  expect_verified(*sim);
}

TEST(Qtegra, ReadHv) {
  auto sim = open_scripted({step("GetHighVoltage\r", "4499.8\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto v = q.read_hv();
  ASSERT_TRUE(v) << to_string(v.error());
  EXPECT_DOUBLE_EQ(*v, 4499.8);
  expect_verified(*sim);
}

TEST(Qtegra, OutOfRangeHvIsConfigAndWritesNothing) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  const double inf = std::numeric_limits<double>::infinity();
  for (double v : {-0.001, 10000.001, std::numeric_limits<double>::quiet_NaN(), inf, -inf}) {
    expect_config(q.set_hv(v));
    expect_config(q.set_param(SourceParam::HV, v));
  }
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, ParamsCoverCodecMapWithNominalRanges) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  const auto specs = q.params();
  // One spec per canonical parameter, under the codec's preferred hardware name.
  EXPECT_EQ(specs.size(), source_params().size());
  for (const auto& info : source_params()) {
    const ParamSpec* spec = find_spec(specs, ParamId{info.param});
    ASSERT_NE(spec, nullptr) << info.name;
    EXPECT_EQ(spec->vendor_name, *codec::qtegra::hardware_name(info.name)) << info.name;
    EXPECT_TRUE(spec->readable && spec->writable) << info.name;
    if (info.param == SourceParam::HV) {
      EXPECT_EQ(spec->range, (Range{0.0, 10000.0}));
      EXPECT_EQ(spec->unit, Unit::Volts);
    } else {
      EXPECT_EQ(spec->range, (Range{-1e6, 1e6})) << info.name;
      EXPECT_EQ(spec->unit, Unit::None) << info.name;
    }
  }
}

TEST(Qtegra, SetParamUsesHardwareName) {
  auto sim = open_scripted({step("SetParameter Trap Current Set,200\r", "OK\r\n"),
                            step("SetParameter Y-Symmetry Set,-12.5\r", "OK\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_TRUE(q.set_param(SourceParam::TrapCurrent, 200.0));
  EXPECT_TRUE(q.set_param(SourceParam::YSymmetry, -12.5));
  expect_verified(*sim);
}

TEST(Qtegra, SetParamNonOkIsProtocol) {
  auto sim = open_scripted({step("SetParameter Trap Current Set,200\r", "200\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto r = q.set_param(SourceParam::TrapCurrent, 200.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "argus");
}

TEST(Qtegra, SetParamOutsideNominalRangeIsConfigAndWritesNothing) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  expect_config(q.set_param(SourceParam::TrapCurrent, 1.0000001e6));
  expect_config(q.set_param(SourceParam::TrapCurrent, std::numeric_limits<double>::quiet_NaN()));
  expect_config(q.set_param(Custom{"Rotation Quad"}, std::numeric_limits<double>::infinity()));
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, ReadParamWithReadbackName) {
  auto sim = open_scripted({step("GetParameter Trap Current Set\r", "200\r\n"),
                            step("GetParameter Trap Current Readback\r", "198.7\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto rb = q.read_param(SourceParam::TrapCurrent);
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{200.0, 198.7}));
  expect_verified(*sim);
}

TEST(Qtegra, ReadParamReadbackFailureIsReturned) {
  auto sim = open_scripted({step("GetParameter Trap Current Set\r", "200\r\n"),
                            step("GetParameter Trap Current Readback\r", "ERROR: bad\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto rb = q.read_param(SourceParam::TrapCurrent);
  ASSERT_FALSE(rb);
  EXPECT_EQ(rb.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(rb.error().device, "argus");
}

TEST(Qtegra, ReadParamWithoutReadbackNameHasNoActual) {
  auto sim = open_scripted({step("GetParameter Y-Symmetry Set\r", "-12.5\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  auto rb = q.read_param(SourceParam::YSymmetry);
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{-12.5, std::nullopt}));
  expect_verified(*sim);
}

// A Custom id names a hardware name the codec knows outside params(): here
// the alias HelixSource writes the rotation quad under.
TEST(Qtegra, CustomParamRoundTrip) {
  auto sim = open_scripted({step("SetParameter Rotation Quad,1.5\r", "OK\r\n"),
                            step("GetParameter Rotation Quad\r", "1.5\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  const ParamId id{Custom{"Rotation Quad"}};
  ASSERT_TRUE(q.set_param(id, 1.5));
  auto rb = q.read_param(id);
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{1.5, std::nullopt}));
  expect_verified(*sim);
}

// HV is written with SetHV and read with GetHighVoltage however it is named.
TEST(Qtegra, HvParamUsesSetHvAndGetHighVoltage) {
  auto sim = open_scripted({step("SetHV 4500\r", "OK\r\n"), step("GetHighVoltage\r", "4499.8\r\n"),
                            step("SetHV 9900\r", "OK\r\n"), step("GetHighVoltage\r", "9899.5\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  ASSERT_TRUE(q.set_param(SourceParam::HV, 4500.0));
  auto rb = q.read_param(SourceParam::HV);
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{4499.8, 4499.8}));
  ASSERT_TRUE(q.set_param(Custom{"HV"}, 9900.0));
  rb = q.read_param(Custom{"HV"});
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{9899.5, 9899.5}));
  expect_verified(*sim);
}

TEST(Qtegra, CustomNameIsCheckedAgainstItsCanonicalRange) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  expect_config(q.set_param(Custom{"HV"}, 50000.0));
  expect_config(q.set_param(Custom{"HV"}, -1.0));
  expect_config(q.set_param(Custom{"Trap Current Set"}, 2e6));
  expect_config(q.set_param(Custom{"Rotation Quad"}, -2e6));  // alias of rotation_quad
  EXPECT_TRUE(sim->written().empty());
}

TEST(Qtegra, CustomReadbackNameIsReadOnly) {
  auto sim = open_scripted({step("GetParameter Trap Current Readback\r", "198.7\r\n")});
  QtegraSpectrometer q("argus", *sim, {});
  const ParamId id{Custom{"Trap Current Readback"}};
  expect_config(q.set_param(id, 200.0));
  EXPECT_TRUE(sim->written().empty());
  auto rb = q.read_param(id);
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{198.7, 198.7}));
  expect_verified(*sim);
}

TEST(Qtegra, UnadvertisedParamIsConfig) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  const ParamId id{Custom{"Filament Glow"}};
  expect_config(q.set_param(id, 1.0));
  expect_config(q.read_param(id));
  EXPECT_TRUE(sim->written().empty());
}

// --- acquirer shape (behaviour is in test_thermo_qtegra_acquire.cpp) -----------

TEST(Qtegra, ChannelsAndIntegratesAreReal) {
  auto sim = open_scripted({});
  QtegraSpectrometer q("argus", *sim, {});
  EXPECT_EQ(q.channels(), (std::vector<ChannelId>{"H2", "H1", "AX", "L1", "L2", "CDD"}));
  EXPECT_TRUE(q.integrates());
}

// --- reconnect -----------------------------------------------------------------

struct QtegraReconnect : ::testing::Test {
  ManualClock clock;
  std::shared_ptr<QtegraSimModel> model = std::make_shared<QtegraSimModel>();
  std::unique_ptr<SimTransport> sim = open_hooked(qtegra_sim_hook(model));
  FlakyTransport flaky{*sim};
  QtegraSpectrometer q{"argus", flaky, {}, &clock};
};

TEST_F(QtegraReconnect, IoErrorReconnectsRunsConnectAndRetries) {
  {
    std::lock_guard lock(model->mutex);
    model->dac = 3.25;
  }
  flaky.fail_exchanges = 1;
  auto v = q.read();
  ASSERT_TRUE(v) << to_string(v.error());
  EXPECT_DOUBLE_EQ(*v, 3.25);
  EXPECT_EQ(q.reconnects(), 1U);
  EXPECT_EQ(flaky.closes, 1);
  EXPECT_EQ(flaky.opens, 1);
  // The dropped exchange never reached the wire; connect ran before the retry.
  EXPECT_EQ(written_text(*sim), (std::vector<std::string>{"GetIntegrationTime\r", "GetMagnetDAC\r"}));
}

// The connect step runs inside next() here, so it must not need anything
// next() holds; it also re-seeds the period from what the instrument reports.
TEST_F(QtegraReconnect, ReconnectInsideNextDeliversTheFrame) {
  {
    std::lock_guard lock(model->mutex);
    model->intensities = {{"H1", 1.5}};
    model->integration_s = 2.097152;
  }
  ASSERT_TRUE(q.start());
  flaky.fail_exchanges = 1;
  auto frame = q.next(Duration::zero());
  ASSERT_TRUE(frame) << to_string(frame.error());
  ASSERT_TRUE(frame->has_value());
  EXPECT_EQ((*frame)->values, (std::vector<std::pair<ChannelId, double>>{{"H1", 1.5}}));
  EXPECT_EQ(q.reconnects(), 1U);
  EXPECT_EQ(written_text(*sim), (std::vector<std::string>{"GetIntegrationTime\r", "GetData\r"}));
  // 2 s snaps to the re-seeded 2.097152 s: nothing is written.
  ASSERT_TRUE(q.configure(2s));
  EXPECT_EQ(sim->written().size(), 2U);
}

TEST_F(QtegraReconnect, ConnectIoErrorIsReturnedWithoutReconnecting) {
  flaky.fail_exchanges = 1;
  auto r = q.connect();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(q.reconnects(), 0U);
  EXPECT_EQ(flaky.opens, 0);
  EXPECT_TRUE(sim->written().empty());
}

TEST_F(QtegraReconnect, FailedConnectDuringReconnectIsReturnedOnce) {
  // Both the command and the connect step after the reopen fail: the connect
  // step's error comes back and nothing recurses.
  flaky.fail_exchanges = 2;
  auto v = q.read();
  ASSERT_FALSE(v);
  EXPECT_EQ(v.error().kind, ErrorKind::Io);
  EXPECT_EQ(v.error().device, sim->name());
  EXPECT_EQ(q.reconnects(), 0U);
  EXPECT_EQ(flaky.opens, 1);
  EXPECT_TRUE(sim->written().empty());
}

// --- sim hook ------------------------------------------------------------------

TEST(QtegraSimHook, SimHookAnswersPositionerBlankAndDetectorCommands) {
  ManualClock clock;
  auto model = std::make_shared<QtegraSimModel>();
  model->clock = &clock;
  model->move_time = 200ms;
  model->integration_s = 2.097152;
  auto sim = open_hooked(qtegra_sim_hook(model));
  QtegraSpectrometer q("argus", *sim, {}, &clock);

  ASSERT_TRUE(q.connect());
  ASSERT_TRUE(q.set(5.5));
  EXPECT_DOUBLE_EQ(*q.read(), 5.5);
  EXPECT_TRUE(*q.moving());
  clock.advance(199ms);
  EXPECT_TRUE(*q.moving());
  clock.advance(1ms);
  EXPECT_FALSE(*q.moving());

  ASSERT_TRUE(q.blank(true));
  ASSERT_TRUE(q.protect("CDD", true));
  ASSERT_TRUE(q.set_deflection("H1", 12.5));
  ASSERT_TRUE(q.set_gain("AX", 1.002));
  EXPECT_DOUBLE_EQ(*q.read_deflection("H1"), 12.5);
  EXPECT_DOUBLE_EQ(*q.read_gain("AX"), 1.002);
  EXPECT_DOUBLE_EQ(*q.read_deflection("L1"), 0.0);
  {
    std::lock_guard lock(model->mutex);
    EXPECT_DOUBLE_EQ(model->dac, 5.5);
    EXPECT_TRUE(model->blank);
    EXPECT_TRUE(model->protect.at("CDD"));
  }
  ASSERT_TRUE(q.blank(false));
  ASSERT_TRUE(q.protect("CDD", false));
  std::lock_guard lock(model->mutex);
  EXPECT_FALSE(model->blank);
  EXPECT_FALSE(model->protect.at("CDD"));
}

TEST(QtegraSimHook, SimHookLogsEveryCommandInOrder) {
  auto model = std::make_shared<QtegraSimModel>();
  auto hook = qtegra_sim_hook(model);
  // Unknown and malformed commands are logged too; an empty line is not.
  for (std::string_view tx : {"GetIntegrationTime\r", "ProtectDetector CDD,On\r", "BlankBeam True\r",
                              "SetParameter Trap Voltage Set, 5\r\n", "Reset\r", "SetMagnetDAC x\r", "\r"}) {
    (void)hook(to_bytes(tx));
  }
  std::lock_guard lock(model->mutex);
  EXPECT_EQ(model->commands,
            (std::vector<std::string>{"GetIntegrationTime", "ProtectDetector CDD,On", "BlankBeam True",
                                      "SetParameter Trap Voltage Set,5", "Reset", "SetMagnetDAC x"}));
}

TEST(QtegraSimHook, SimHookAnswersSourceAndAcquirerCommands) {
  auto model = std::make_shared<QtegraSimModel>();
  model->intensities = {{"H1", 1.5}, {"AX", -0.25}};
  auto hook = qtegra_sim_hook(model);
  auto ask = [&](std::string_view tx) { return to_string(hook(to_bytes(tx))); };

  EXPECT_EQ(ask("SetHV 4500\r"), "OK\r\n");
  EXPECT_EQ(ask("GetHighVoltage\r"), "4500\r\n");
  EXPECT_EQ(ask("SetParameter Trap Current Set,200\r"), "OK\r\n");
  EXPECT_EQ(ask("GetParameter Trap Current Set\r"), "200\r\n");
  // A readback name reports its set name's value until the model holds one.
  EXPECT_EQ(ask("GetParameter Trap Current Readback\r"), "200\r\n");
  EXPECT_EQ(ask("GetParameter Never Set\r"), "0\r\n");
  EXPECT_EQ(ask("SetIntegrationTime 2.097152\r"), "OK\r\n");
  EXPECT_EQ(ask("GetIntegrationTime\r"), "2.097152\r\n");
  EXPECT_EQ(ask("GetData\r"), "AX,-0.25,H1,1.5\r\n");
  {
    std::lock_guard lock(model->mutex);
    EXPECT_DOUBLE_EQ(model->hv, 4500.0);
    EXPECT_DOUBLE_EQ(model->params.at("Trap Current Set"), 200.0);
    model->params["Trap Current Readback"] = 198.7;
    model->data_override = "H1,nan";
  }
  EXPECT_EQ(ask("GetParameter Trap Current Readback\r"), "198.7\r\n");
  EXPECT_EQ(ask("GetData\r"), "H1,nan\r\n");
}

TEST(QtegraSimHook, UnknownOrMalformedCommandAnswersError) {
  auto hook = qtegra_sim_hook(std::make_shared<QtegraSimModel>());
  for (std::string_view tx : {"Reset\r", "SetMagnetDAC\r", "SetMagnetDAC x\r", "ProtectDetector CDD,Maybe\r", "\r",
                              "SetHV\r", "SetParameter Trap Current Set\r", "SetIntegrationTime fast\r",
                              "GetParameter\r", "GetData\r"}) {
    auto reply = codec::qtegra::decode_number(hook(to_bytes(tx)));
    ASSERT_FALSE(reply) << tx;
    EXPECT_EQ(reply.error().kind, ErrorKind::Protocol) << tx;
    EXPECT_NE(reply.error().what.find("device error"), std::string::npos) << tx;
  }
}
