#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include "pychron/systems/spectrometer/config_loader.hpp"

namespace pychron::spectrometer::cfg {
namespace {

// Spec section 7.1 verbatim, plus one detector and the acquirer channel list.
constexpr std::string_view kIntegrated = R"toml(
[system]
name = "jan-argus"
reference_detector = "H1"
integration_time_s = 1.0

[transports.qtegra]
kind = "tcp"
host = "192.168.0.10"
port = 1069
timeout_ms = 2000

[drivers.qtegra]
kind = "thermo_qtegra"
transport = "qtegra"
roles = ["positioner", "source", "acquirer", "detector_control", "beam_blank"]
channels = ["H1", "CDD"]

[magnet]
positioner = "qtegra"
native_axis = "dac"
limits = { min = 0.0, max = 10.0 }
settle_ms = 500
field_table = "argon"
hv_table = "argon_hv"
corrections = { deflection = true, hv = true }
protection = { detectors = ["CDD"], beam_blank_threshold = 0.5 }
af_demag = { enabled = false, period_s = 0.5, duration_s = 10, start_amplitude = 0.5, threshold = 0.5 }

[source]
driver = "qtegra"
nominal_hv = 4500
ramp = { TrapCurrent = 10.0, HV = 200.0 }

[acquisition]
acquirers = ["qtegra"]
stale_frame_guard = true
timeout_factor = 2.0

[detector_control]
driver = "qtegra"

[[detectors]]
name = "H1"
kind = "faraday"
channel = "qtegra:H1"
units = "fA"
software_gain = 1.0
isotope = "Ar40"
active = true
deflection = { control = true, correction = [0.0, 0.0012], sign = 1, max = 800, per_volt = 0.0031 }
protection = { threshold = 5e5, on_move = true }
saturation = 4.9e6

[[detectors]]
name = "CDD"
kind = "counter"
channel = "qtegra:CDD"
units = "cps"
dead_time_ns = 25
cdd_voltage = 1450
protection = { threshold = 5e5 }
)toml";

ConfigLoadReport parse(std::string_view toml) { return parse_config_from_string(toml, "spectrometer.toml"); }

std::string dump(const ConfigLoadReport& r) {
  std::string out;
  for (const auto& d : r.diagnostics) out += config::to_string(d) + "\n";
  return out;
}

bool mentions(const ConfigLoadReport& r, std::string_view field, std::string_view text) {
  for (const auto& d : r.diagnostics) {
    if (d.field == field && d.message.find(text) != std::string::npos) return true;
  }
  return false;
}

// Replaces the first occurrence of `from` in the integrated fixture.
std::string with(std::string_view from, std::string_view to) {
  std::string s(kIntegrated);
  auto pos = s.find(from);
  EXPECT_NE(pos, std::string::npos) << from;
  if (pos != std::string::npos) s.replace(pos, from.size(), to);
  return s;
}

TEST(SpectrometerConfig, QtegraOverUdp) {
  // ldeo and felix reach Qtegra over UDP (legacy survey C.6).
  auto r = parse(with("kind = \"tcp\"", "kind = \"udp\""));
  ASSERT_TRUE(r.ok()) << dump(r);
  const auto& t = r.config->transports.at("qtegra");
  EXPECT_EQ(t.kind, TransportKind::Udp);
  EXPECT_EQ(t.host, "192.168.0.10");
  EXPECT_EQ(t.tcp_port, 1069);
}

TEST(SpectrometerConfig, ParsesIntegratedSpecExample) {
  auto r = parse(kIntegrated);
  ASSERT_TRUE(r.ok()) << dump(r);
  const auto& c = *r.config;
  EXPECT_EQ(c.system.name, "jan-argus");
  EXPECT_EQ(c.system.reference_detector, "H1");
  EXPECT_DOUBLE_EQ(c.system.integration_time_s, 1.0);

  const auto& t = c.transports.at("qtegra");
  EXPECT_EQ(t.kind, TransportKind::Tcp);
  EXPECT_EQ(t.host, "192.168.0.10");
  EXPECT_EQ(t.tcp_port, 1069);
  EXPECT_EQ(t.timeout_ms, 2000);

  const auto* d = c.driver("qtegra");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->kind, "thermo_qtegra");
  EXPECT_EQ(d->roles.size(), 5u);
  EXPECT_TRUE(d->has_role(Role::BeamBlank));
  EXPECT_EQ(d->channels, (std::vector<std::string>{"H1", "CDD"}));

  EXPECT_EQ(c.magnet.positioner, "qtegra");
  EXPECT_EQ(c.magnet.native_axis, Axis::Dac);
  ASSERT_TRUE(c.magnet.limits);
  EXPECT_DOUBLE_EQ(c.magnet.limits->max, 10.0);
  EXPECT_EQ(c.magnet.settle_ms, 500);
  EXPECT_EQ(c.magnet.field_table, "argon");
  EXPECT_EQ(c.magnet.hv_table, "argon_hv");
  EXPECT_TRUE(c.magnet.corrections.deflection);
  EXPECT_TRUE(c.magnet.corrections.hv);
  EXPECT_EQ(c.magnet.protection.detectors, (std::vector<std::string>{"CDD"}));
  EXPECT_EQ(c.magnet.protection.beam_blank_threshold, 0.5);
  EXPECT_FALSE(c.magnet.af_demag.enabled);
  EXPECT_DOUBLE_EQ(c.magnet.af_demag.duration_s, 10.0);

  EXPECT_EQ(c.source.driver, "qtegra");
  EXPECT_EQ(c.source.nominal_hv, 4500.0);
  EXPECT_DOUBLE_EQ(c.source.ramp.at("TrapCurrent"), 10.0);
  EXPECT_DOUBLE_EQ(c.source.ramp.at("HV"), 200.0);

  EXPECT_EQ(c.acquisition.acquirers, (std::vector<std::string>{"qtegra"}));
  EXPECT_TRUE(c.acquisition.stale_frame_guard);
  EXPECT_DOUBLE_EQ(c.acquisition.timeout_factor, 2.0);
  EXPECT_FALSE(c.acquisition.host_integration);
  ASSERT_TRUE(c.detector_control);
  EXPECT_EQ(c.detector_control->driver, "qtegra");

  ASSERT_EQ(c.detectors.size(), 2u);
  const auto& h1 = c.detectors[0];
  EXPECT_EQ(h1.name, "H1");
  EXPECT_EQ(h1.kind, DetectorKind::Faraday);
  EXPECT_EQ(h1.channel, "qtegra:H1");
  EXPECT_EQ(h1.isotope, "Ar40");
  ASSERT_TRUE(h1.deflection);
  EXPECT_TRUE(h1.deflection->control);
  EXPECT_EQ(h1.deflection->correction, (std::vector<double>{0.0, 0.0012}));
  EXPECT_EQ(h1.deflection->max, 800.0);
  ASSERT_TRUE(h1.protection);
  EXPECT_DOUBLE_EQ(h1.protection->threshold, 5e5);
  EXPECT_TRUE(h1.protection->on_move);
  EXPECT_EQ(h1.saturation, 4.9e6);
  const auto& cdd = c.detectors[1];
  EXPECT_EQ(cdd.kind, DetectorKind::Counter);
  EXPECT_EQ(cdd.dead_time_ns, 25.0);
  EXPECT_EQ(cdd.cdd_voltage, 1450.0);
  EXPECT_TRUE(cdd.active);
}

TEST(SpectrometerConfig, DriverKeepsExtraKeysAsOptions) {
  auto r = parse(with("channels = [\"H1\", \"CDD\"]", "channels = [\"H1\", \"CDD\"]\nsample_hz = 100"));
  ASSERT_TRUE(r.ok()) << dump(r);
  const auto* d = r.config->driver("qtegra");
  ASSERT_NE(d->options.get("sample_hz"), nullptr);
  EXPECT_EQ(d->options.get("sample_hz")->value_or<std::int64_t>(0), 100);
  EXPECT_EQ(d->options.get("roles"), nullptr);
}

TEST(SpectrometerConfig, SerialTransportTakesDevicePath) {
  auto r = parse(with("kind = \"tcp\"\nhost = \"192.168.0.10\"\nport = 1069",
                      "kind = \"serial\"\nport = \"/dev/tty.usbserial-H\"\nbaud = 115200"));
  ASSERT_TRUE(r.ok()) << dump(r);
  const auto& t = r.config->transports.at("qtegra");
  EXPECT_EQ(t.kind, TransportKind::Serial);
  EXPECT_EQ(t.serial_port, "/dev/tty.usbserial-H");
  EXPECT_EQ(t.baud, 115200);
}

TEST(SpectrometerConfig, SyntaxErrorIsReportedWithLine) {
  auto r = parse("[system\nname = 1");
  ASSERT_FALSE(r.ok());
  ASSERT_EQ(r.diagnostics.size(), 1u);
  EXPECT_EQ(r.diagnostics[0].field, "toml");
  EXPECT_EQ(r.diagnostics[0].loc.line, 1u);
}

TEST(SpectrometerConfig, UnknownSectionAndKeyAreErrors) {
  auto r = parse(std::string(kIntegrated) + "\n[bogus]\nx = 1\n");
  EXPECT_TRUE(mentions(r, "bogus", "unknown section")) << dump(r);
  auto r2 = parse(with("settle_ms = 500", "settle_ms = 500\nsettle = 1"));
  EXPECT_TRUE(mentions(r2, "magnet.settle", "unknown field")) << dump(r2);
}

TEST(SpectrometerConfig, MissingRequiredFieldsAreAllCollected) {
  auto r = parse(with("positioner = \"qtegra\"\nnative_axis = \"dac\"", "limits_note = 0"));
  EXPECT_TRUE(mentions(r, "magnet.positioner", "missing required field")) << dump(r);
  EXPECT_TRUE(mentions(r, "magnet.native_axis", "missing required field")) << dump(r);
}

TEST(SpectrometerConfig, MissingSectionsAreReported) {
  auto r = parse("[system]\nname = \"x\"\nreference_detector = \"H1\"\n");
  EXPECT_TRUE(mentions(r, "magnet", "missing required section")) << dump(r);
  EXPECT_TRUE(mentions(r, "source", "missing required section")) << dump(r);
  EXPECT_TRUE(mentions(r, "acquisition", "missing required section")) << dump(r);
  EXPECT_TRUE(mentions(r, "detectors", "at least one detector")) << dump(r);
}

TEST(SpectrometerConfig, EnumValuesAreChecked) {
  EXPECT_TRUE(mentions(parse(with("native_axis = \"dac\"", "native_axis = \"volts\"")), "magnet.native_axis",
                       "must be one of"));
  EXPECT_TRUE(mentions(parse(with("kind = \"faraday\"", "kind = \"cup\"")), "detectors[0].kind", "must be one of"));
  EXPECT_TRUE(mentions(parse(with("kind = \"tcp\"", "kind = \"carrier_pigeon\"")), "transports.qtegra.kind",
                       "must be one of"));
  EXPECT_TRUE(mentions(parse(with("\"beam_blank\"]", "\"coffee\"]")), "drivers.qtegra.roles[4]", "unknown role"));
  EXPECT_TRUE(mentions(parse(with("timeout_factor = 2.0", "timeout_factor = 2.0\nbin_epoch = \"weekly\"")),
                       "acquisition.bin_epoch", "must be one of"));
}

TEST(SpectrometerConfig, TypeAndRangeErrors) {
  EXPECT_TRUE(mentions(parse(with("settle_ms = 500", "settle_ms = -1")), "magnet.settle_ms", "out of range"));
  EXPECT_TRUE(mentions(parse(with("settle_ms = 500", "settle_ms = \"fast\"")), "magnet.settle_ms",
                       "expected integer"));
  EXPECT_TRUE(mentions(parse(with("integration_time_s = 1.0", "integration_time_s = 0.0")),
                       "system.integration_time_s", "must be > 0"));
  EXPECT_TRUE(mentions(parse(with("limits = { min = 0.0, max = 10.0 }", "limits = { min = 10.0, max = 1.0 }")),
                       "magnet.limits", "min must be < max"));
  EXPECT_TRUE(mentions(parse(with("sign = 1", "sign = 2")), "detectors[0].deflection.sign", "must be 1 or -1"));
  EXPECT_TRUE(mentions(parse(with("timeout_factor = 2.0", "timeout_factor = 0.5")), "acquisition.timeout_factor",
                       "must be >= 1"));
}

TEST(SpectrometerConfig, RampNamesMustBeCanonicalSourceParams) {
  auto r = parse(with("ramp = { TrapCurrent = 10.0, HV = 200.0 }", "ramp = { Trap = 10.0, HV = -1.0 }"));
  EXPECT_TRUE(mentions(r, "source.ramp.Trap", "unknown source parameter")) << dump(r);
  EXPECT_TRUE(mentions(r, "source.ramp.HV", "must be > 0")) << dump(r);
}

TEST(SpectrometerConfig, ChannelMustBeDriverColonChannel) {
  auto r = parse(with("channel = \"qtegra:H1\"", "channel = \"H1\""));
  EXPECT_TRUE(mentions(r, "detectors[0].channel", "<driver>:<channel>")) << dump(r);
}

TEST(SpectrometerConfig, LocalOverrideSetsHostAndPort) {
  auto r = parse_config_from_string(kIntegrated, "spectrometer.toml",
                                    "[transports.qtegra]\nhost = \"10.0.0.5\"\nport = 2000\n", "spectrometer.local.toml");
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.config->transports.at("qtegra").host, "10.0.0.5");
  EXPECT_EQ(r.config->transports.at("qtegra").tcp_port, 2000);
}

TEST(SpectrometerConfig, LocalOverrideMayOnlyTouchTransportConnectionKeys) {
  auto r = parse_config_from_string(kIntegrated, "spectrometer.toml",
                                    "[magnet]\nsettle_ms = 1\n[transports.qtegra]\nkind = \"serial\"\n"
                                    "[transports.ghost]\nhost = \"x\"\n",
                                    "spectrometer.local.toml");
  EXPECT_TRUE(mentions(r, "magnet", "local override may only set")) << dump(r);
  EXPECT_TRUE(mentions(r, "transports.qtegra.kind", "may not be overridden")) << dump(r);
  EXPECT_TRUE(mentions(r, "transports.ghost", "unknown transport")) << dump(r);
}

// This machine's credentials (an NGX login) may live in the local file too.
TEST(SpectrometerConfig, LocalOverrideMaySetDriverCredentialsOnly) {
  auto r = parse_config_from_string(kIntegrated, "spectrometer.toml",
                                    "[drivers.qtegra]\nuser = \"pychron\"\npassword = \"s3cret\"\n",
                                    "spectrometer.local.toml");
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.config->drivers.at("qtegra").options["password"].value<std::string>(), "s3cret");
  auto bad = parse_config_from_string(kIntegrated, "spectrometer.toml",
                                      "[drivers.qtegra]\nkind = \"other\"\n[drivers.ghost]\nuser = \"x\"\n",
                                      "spectrometer.local.toml");
  EXPECT_TRUE(mentions(bad, "drivers.qtegra.kind", "may not be overridden")) << dump(bad);
  EXPECT_TRUE(mentions(bad, "drivers.ghost", "unknown driver")) << dump(bad);
}

TEST(SpectrometerConfig, TransportRetriesAndTraceParsed) {
  auto r = parse(with("port = 1069", "port = 1069\nretries = 3\ntrace = true"));
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.config->transports.at("qtegra").retries, 3);
  EXPECT_TRUE(r.config->transports.at("qtegra").trace);
}

TEST(SpectrometerConfig, TransportRetriesDefaultZeroTraceFalse) {
  auto r = parse(kIntegrated);
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.config->transports.at("qtegra").retries, 0);
  EXPECT_FALSE(r.config->transports.at("qtegra").trace);
}

TEST(SpectrometerConfig, NegativeRetriesIsDiagnostic) {
  auto r = parse(with("port = 1069", "port = 1069\nretries = -1"));
  EXPECT_TRUE(mentions(r, "transports.qtegra.retries", "out of range")) << dump(r);
}

TEST(SpectrometerConfig, TraceMustBeBoolean) {
  auto r = parse(with("port = 1069", "port = 1069\ntrace = \"yes\""));
  EXPECT_TRUE(mentions(r, "transports.qtegra.trace", "boolean")) << dump(r);
}

TEST(SpectrometerConfig, LocalFileCannotSetRetriesOrTrace) {
  auto r = parse_config_from_string(kIntegrated, "spectrometer.toml",
                                    "[transports.qtegra]\nretries = 2\ntrace = true\n", "spectrometer.local.toml");
  EXPECT_TRUE(mentions(r, "transports.qtegra.retries", "may not be overridden")) << dump(r);
  EXPECT_TRUE(mentions(r, "transports.qtegra.trace", "may not be overridden")) << dump(r);
}

TEST(SpectrometerConfig, ParseChannelRef) {
  auto ref = parse_channel_ref("faradays:H1");
  ASSERT_TRUE(ref);
  EXPECT_EQ(ref->driver, "faradays");
  EXPECT_EQ(ref->channel, "H1");
  EXPECT_FALSE(parse_channel_ref("H1"));
  EXPECT_FALSE(parse_channel_ref(":H1"));
  EXPECT_FALSE(parse_channel_ref("x:"));
}

std::string with_color(std::string_view value) { return with("isotope = \"Ar40\"", "isotope = \"Ar40\"\ncolor = " + std::string(value)); }

TEST(DetectorColor, DetectorColorParsed) {
  auto r = parse(with_color("\"#1E90ff\""));
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_EQ(r.config->detectors[0].color, "#1e90ff");
}

TEST(DetectorColor, DetectorColorDefaultsEmpty) {
  auto r = parse(std::string(kIntegrated));
  ASSERT_TRUE(r.ok()) << dump(r);
  EXPECT_TRUE(r.config->detectors[0].color.empty());
}

TEST(DetectorColor, BadDetectorColorIsDiagnosticWithLine) {
  for (const char* bad : {"\"red\"", "\"#12345\"", "\"#12345g\"", "42"}) {
    const auto text = with_color(bad);
    const auto r = parse(text);
    ASSERT_FALSE(r.ok()) << bad;
    const auto key_line = 1u + static_cast<unsigned>(std::count(text.begin(), text.begin() + text.find("\ncolor ") + 1, '\n'));
    bool found = false;
    for (const auto& d : r.diagnostics) {
      if (d.field.size() >= 6 && d.field.compare(d.field.size() - 6, 6, ".color") == 0) {
        found = true;
        EXPECT_EQ(d.loc.line, key_line) << bad;
      }
    }
    EXPECT_TRUE(found) << bad << "\n" << dump(r);
  }
}

TEST(DetectorColor, ExampleConfigsHaveDistinctColors) {
  for (const char* name : {"spectrometer.sim-integrated.toml", "spectrometer.sim-legacy.toml"}) {
    const auto r = parse_config(std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / name);
    ASSERT_TRUE(r.ok()) << name << "\n" << dump(r);
    std::set<std::string> seen;
    for (const auto& d : r.config->detectors) {
      EXPECT_FALSE(d.color.empty()) << name << " " << d.name;
      EXPECT_TRUE(seen.insert(d.color).second) << name << " duplicate colour on " << d.name;
    }
  }
}

}  // namespace
}  // namespace pychron::spectrometer::cfg
