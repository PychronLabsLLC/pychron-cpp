#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "pychron/systems/spectrometer/config_loader.hpp"
#include "pychron/systems/spectrometer/config_validate.hpp"

namespace pychron::spectrometer::cfg {
namespace {

// A valid legacy-split config (spec section 7.2 shape) with one detector that
// uses deflection control, so every rule has something to bite on.
constexpr std::string_view kValid = R"toml(
[system]
name = "vg"
reference_detector = "H1"

[transports.dac]
kind = "labjack_u3"
[transports.adc]
kind = "modbus_tcp"
host = "192.168.0.20"
[transports.counter]
kind = "serial"
port = "/dev/tty.usbserial-C"
baud = 115200
[transports.hv]
kind = "serial"
port = "/dev/tty.usbserial-H"

[drivers.magnet_dac]
kind = "dac_positioner"
transport = "dac"
roles = ["positioner"]
channel = 0
[drivers.faradays]
kind = "adc_bank"
transport = "adc"
roles = ["acquirer"]
sample_hz = 100
channels = ["AX", "H1", "L1"]
[drivers.multiplier]
kind = "pulse_counter"
transport = "counter"
roles = ["acquirer"]
channels = ["EM"]
[drivers.spellman]
kind = "serial_hv"
transport = "hv"
roles = ["source"]
[drivers.deflector]
kind = "sim_integrated"
transport = "hv"
roles = ["detector_control"]

[magnet]
positioner = "magnet_dac"
native_axis = "dac"
field_table = "argon"
corrections = { deflection = true, hv = true }
protection = { detectors = ["EM"] }

[source]
driver = "spellman"
nominal_hv = 4500

[acquisition]
acquirers = ["faradays", "multiplier"]
host_integration = true
bin_epoch = "shared"
ignored_channels = ["faradays:L1"]

[detector_control]
driver = "deflector"

[[detectors]]
name = "H1"
kind = "faraday"
channel = "faradays:H1"
deflection = { control = true, correction = [0.0, 0.001] }

[[detectors]]
name = "AX"
kind = "faraday"
channel = "faradays:AX"

[[detectors]]
name = "EM"
kind = "counter"
channel = "multiplier:EM"
protection = { threshold = 1e6, on_move = true }
)toml";

std::string dump(const Diagnostics& ds) {
  std::string out;
  for (const auto& d : ds) out += config::to_string(d) + "\n";
  return out;
}

bool mentions(const Diagnostics& ds, std::string_view field, std::string_view text) {
  for (const auto& d : ds) {
    if (d.field == field && d.message.find(text) != std::string::npos) return true;
  }
  return false;
}

std::string with(std::string_view from, std::string_view to) {
  std::string s(kValid);
  auto pos = s.find(from);
  EXPECT_NE(pos, std::string::npos) << from;
  if (pos != std::string::npos) s.replace(pos, from.size(), to);
  return s;
}

SpectrometerConfig parse(std::string_view toml) {
  auto r = parse_config_from_string(toml, "spectrometer.toml");
  EXPECT_TRUE(r.ok()) << dump(r.diagnostics);
  return r.ok() ? std::move(*r.config) : SpectrometerConfig{};
}

TableFile table(std::string name, Axis axis, std::vector<std::string> dets) {
  TableFile t;
  t.name = std::move(name);
  t.axis = axis;
  TablePoint p{"Ar40", 39.962, {}};
  for (auto& d : dets) p.values[d] = 5.0;
  t.points.push_back(p);
  return t;
}

Tables good_tables() { return {{"argon", table("argon", Axis::Dac, {"H1", "AX", "EM"})}}; }

TEST(SpectrometerValidate, ValidFixturePassesEveryRule) {
  auto c = parse(kValid);
  auto ds = validate(c, good_tables());
  EXPECT_TRUE(ds.empty()) << dump(ds);
}

// ---- check_references -----------------------------------------------------

TEST(SpectrometerValidate, References_DanglingTransportAndReferenceDetector) {
  auto c = parse(with("transport = \"hv\"\nroles = [\"source\"]", "transport = \"hvx\"\nroles = [\"source\"]"));
  auto ds = check_references(c);
  EXPECT_TRUE(mentions(ds, "drivers.spellman.transport", "unknown transport 'hvx'")) << dump(ds);

  auto c2 = parse(with("reference_detector = \"H1\"", "reference_detector = \"H9\""));
  EXPECT_TRUE(mentions(check_references(c2), "system.reference_detector", "unknown detector 'H9'"));
}

TEST(SpectrometerValidate, References_DuplicateDetectorName) {
  auto c = parse(with("name = \"AX\"", "name = \"H1\""));
  auto ds = check_references(c);
  EXPECT_TRUE(mentions(ds, "detectors[1].name", "duplicate detector 'H1'")) << dump(ds);
}

// ---- check_roles ----------------------------------------------------------

TEST(SpectrometerValidate, Roles_ReferencedRoleMissingOnDriver) {
  auto c = parse(with("driver = \"spellman\"", "driver = \"magnet_dac\""));
  auto ds = check_roles(c);
  EXPECT_TRUE(mentions(ds, "source.driver", "driver 'magnet_dac' does not have role 'source'")) << dump(ds);
}

TEST(SpectrometerValidate, Roles_UnknownDriverAndAcquirerWithoutChannels) {
  auto c = parse(with("acquirers = [\"faradays\", \"multiplier\"]", "acquirers = [\"faradays\", \"spellman\", \"nope\"]"));
  auto ds = check_roles(c);
  EXPECT_TRUE(mentions(ds, "acquisition.acquirers[1]", "does not have role 'acquirer'")) << dump(ds);
  EXPECT_TRUE(mentions(ds, "acquisition.acquirers[2]", "unknown driver 'nope'")) << dump(ds);

  auto c2 = parse(with("channels = [\"EM\"]", "sample_hz = 1"));
  EXPECT_TRUE(mentions(check_roles(c2), "drivers.multiplier.channels", "acquirer must declare"));
}

TEST(SpectrometerValidate, Roles_DetectorControlAndPositioner) {
  auto c = parse(with("positioner = \"magnet_dac\"", "positioner = \"faradays\""));
  EXPECT_TRUE(mentions(check_roles(c), "magnet.positioner", "does not have role 'positioner'"));
  auto c2 = parse(with("driver = \"deflector\"", "driver = \"spellman\""));
  EXPECT_TRUE(mentions(check_roles(c2), "detector_control.driver", "does not have role 'detector_control'"));
}

// ---- check_channels -------------------------------------------------------

TEST(SpectrometerValidate, Channels_DetectorChannelMustResolve) {
  auto c = parse(with("channel = \"faradays:AX\"", "channel = \"faradays:ZZ\""));
  auto ds = check_channels(c);
  EXPECT_TRUE(mentions(ds, "detectors[1].channel", "no acquirer channel 'faradays:ZZ'")) << dump(ds);
  // AX is now neither bound nor ignored.
  EXPECT_TRUE(mentions(ds, "drivers.faradays.channels", "'faradays:AX' is not bound")) << dump(ds);

  auto c2 = parse(with("channel = \"faradays:AX\"", "channel = \"spellman:AX\""));
  EXPECT_TRUE(mentions(check_channels(c2), "detectors[1].channel", "not an acquirer"));
}

TEST(SpectrometerValidate, Channels_TwoDetectorsOnOneChannel) {
  auto c = parse(with("channel = \"faradays:AX\"", "channel = \"faradays:H1\""));
  auto ds = check_channels(c);
  EXPECT_TRUE(mentions(ds, "detectors[1].channel", "already bound to detector 'H1'")) << dump(ds);
}

TEST(SpectrometerValidate, Channels_UnboundAcquirerChannel) {
  auto c = parse(with("ignored_channels = [\"faradays:L1\"]", "ignored_channels = []"));
  auto ds = check_channels(c);
  EXPECT_TRUE(mentions(ds, "drivers.faradays.channels", "'faradays:L1' is not bound")) << dump(ds);
}

TEST(SpectrometerValidate, Channels_IgnoredChannelMustExistAndNotBeBound) {
  auto c = parse(with("ignored_channels = [\"faradays:L1\"]", "ignored_channels = [\"faradays:L1\", \"faradays:Q\", \"faradays:H1\"]"));
  auto ds = check_channels(c);
  EXPECT_TRUE(mentions(ds, "acquisition.ignored_channels[1]", "no acquirer channel")) << dump(ds);
  EXPECT_TRUE(mentions(ds, "acquisition.ignored_channels[2]", "bound to detector 'H1'")) << dump(ds);
}

TEST(SpectrometerValidate, Channels_DuplicateChannelOnDriver) {
  auto c = parse(with("channels = [\"AX\", \"H1\", \"L1\"]", "channels = [\"AX\", \"H1\", \"L1\", \"H1\"]"));
  EXPECT_TRUE(mentions(check_channels(c), "drivers.faradays.channels[3]", "duplicate channel 'H1'"));
}

TEST(SpectrometerValidate, Channels_SeveralAcquirersNeedHostIntegration) {
  auto c = parse(with("host_integration = true", "host_integration = false"));
  EXPECT_TRUE(mentions(check_channels(c), "acquisition.host_integration", "more than one acquirer"));
}

// ---- check_field_tables ---------------------------------------------------

TEST(SpectrometerValidate, FieldTable_Missing) {
  auto c = parse(kValid);
  auto ds = check_field_tables(c, {});
  EXPECT_TRUE(mentions(ds, "magnet.field_table", "table 'argon' not found")) << dump(ds);
}

TEST(SpectrometerValidate, FieldTable_MissingColumnForActiveDetector) {
  auto c = parse(kValid);
  auto ds = check_field_tables(c, {{"argon", table("argon", Axis::Dac, {"H1", "AX"})}});
  EXPECT_TRUE(mentions(ds, "magnet.field_table", "no column for active detector 'EM'")) << dump(ds);
}

TEST(SpectrometerValidate, FieldTable_InactiveDetectorNeedsNoColumn) {
  auto c = parse(with("channel = \"multiplier:EM\"", "channel = \"multiplier:EM\"\nactive = false"));
  auto ds = check_field_tables(c, {{"argon", table("argon", Axis::Dac, {"H1", "AX"})}});
  EXPECT_TRUE(ds.empty()) << dump(ds);
}

TEST(SpectrometerValidate, FieldTable_WrongAxis) {
  auto c = parse(kValid);
  auto ds = check_field_tables(c, {{"argon", table("argon", Axis::Field, {"H1", "AX", "EM"})}});
  EXPECT_TRUE(mentions(ds, "magnet.field_table", "axis 'field' does not match positioner native_axis 'dac'"))
      << dump(ds);
}

TEST(SpectrometerValidate, FieldTable_HvTableCheckedToo) {
  auto c = parse(with("field_table = \"argon\"", "field_table = \"argon\"\nhv_table = \"argon_hv\""));
  auto ds = check_field_tables(c, good_tables());
  EXPECT_TRUE(mentions(ds, "magnet.hv_table", "table 'argon_hv' not found")) << dump(ds);
}

// ---- check_protection -----------------------------------------------------

TEST(SpectrometerValidate, Protection_DetectorWithoutProtectionConfig) {
  auto c = parse(with("detectors = [\"EM\"]", "detectors = [\"EM\", \"AX\", \"QQ\"]"));
  auto ds = check_protection(c);
  EXPECT_TRUE(mentions(ds, "magnet.protection.detectors[1]", "'AX' has no protection config")) << dump(ds);
  EXPECT_TRUE(mentions(ds, "magnet.protection.detectors[2]", "unknown detector 'QQ'")) << dump(ds);
}

TEST(SpectrometerValidate, Protection_DetectorControlRequiredForDeflectionOrProtection) {
  auto c = parse(with("[detector_control]\ndriver = \"deflector\"", ""));
  auto ds = check_protection(c);
  EXPECT_TRUE(mentions(ds, "detectors[0].deflection", "requires [detector_control]")) << dump(ds);
  EXPECT_TRUE(mentions(ds, "detectors[2].protection", "requires [detector_control]")) << dump(ds);
}

// ---- check_hv_correction --------------------------------------------------

TEST(SpectrometerValidate, HvCorrection_ForbiddenOnMassAxis) {
  auto c = parse(with("native_axis = \"dac\"", "native_axis = \"mass\""));
  auto ds = check_hv_correction(c);
  EXPECT_TRUE(mentions(ds, "magnet.corrections.hv", "native_axis = \"mass\"")) << dump(ds);
}

TEST(SpectrometerValidate, HvCorrection_NeedsNominalHv) {
  auto c = parse(with("nominal_hv = 4500", ""));
  EXPECT_TRUE(mentions(check_hv_correction(c), "source.nominal_hv", "required when magnet.corrections.hv"));
}

// ---- validate -------------------------------------------------------------

TEST(SpectrometerValidate, ValidateCollectsEveryRule) {
  auto s = with("native_axis = \"dac\"", "native_axis = \"mass\"");
  s.replace(s.find("driver = \"spellman\""), 19, "driver = \"faradays\"");
  auto c = parse(s);
  auto ds = validate(c, {});
  EXPECT_TRUE(mentions(ds, "magnet.corrections.hv", "mass")) << dump(ds);
  EXPECT_TRUE(mentions(ds, "source.driver", "does not have role 'source'")) << dump(ds);
  EXPECT_TRUE(mentions(ds, "magnet.field_table", "not found")) << dump(ds);
  // Diagnostics point at the offending line.
  for (const auto& d : ds) EXPECT_GT(d.loc.line, 0u) << config::to_string(d);
}

}  // namespace
}  // namespace pychron::spectrometer::cfg
