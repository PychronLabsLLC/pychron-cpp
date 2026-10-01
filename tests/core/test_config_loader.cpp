#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "config_fixtures.hpp"
#include "pychron/core/config/loader.hpp"

using namespace pychron;
using namespace pychron::config;
using pychron::test::has;
using pychron::test::line_of;

namespace {

// The spec example plus the two valves its pipette refers to.
std::string example_with_pipette_valves() {
  return std::string(test::kExampleConfig) +
         "\n[[valves]]\nname = \"P1\"\nactuator = \"actuator1\"\naddress = \"3\"\n"
         "\n[[valves]]\nname = \"P2\"\nactuator = \"actuator1\"\naddress = \"4\"\n";
}

std::string at(std::string_view text, std::string_view needle, std::string_view rest) {
  return "f.toml:" + std::to_string(line_of(text, needle)) + ":" + std::string(rest);
}

}  // namespace

TEST(ConfigLoader, ParsesSpecExampleIntoTypedStructs) {
  auto r = load_system_config_from_string(example_with_pipette_valves(), "extraction_line.toml");
  ASSERT_TRUE(r) << r.error().what;
  const auto& c = *r;

  EXPECT_EQ(c.system.name, "jan");
  EXPECT_EQ(c.system.scan_interval_ms, 1000);

  const auto& vb = c.transports.at("valve_bus");
  EXPECT_EQ(vb.kind, TransportKind::Serial);
  EXPECT_EQ(vb.timeout_ms, 500);
  EXPECT_EQ(vb.retries, 2);
  EXPECT_TRUE(vb.trace);
  const auto& serial = std::get<SerialParams>(vb.params);
  EXPECT_EQ(serial.port, "/dev/tty.usbserial-A1");
  EXPECT_EQ(serial.baud, 9600);
  EXPECT_EQ(serial.parity, Parity::None);

  const auto& gn = c.transports.at("gauge_net");
  EXPECT_EQ(gn.kind, TransportKind::Tcp);
  EXPECT_EQ(std::get<TcpParams>(gn.params).host, "192.168.0.51");
  EXPECT_EQ(std::get<TcpParams>(gn.params).port, 8000);
  EXPECT_FALSE(gn.trace);

  const auto& ig = c.drivers.at("ig_controller");
  EXPECT_EQ(ig.kind, "pfeiffer_maxigauge");
  EXPECT_EQ(ig.transport, "gauge_net");
  EXPECT_EQ(ig.channels, (std::vector<std::int64_t>{1, 2, 3}));
  EXPECT_EQ(ig.options["kind"].value_or(std::string{}), "pfeiffer_maxigauge");

  ASSERT_EQ(c.valves.size(), 4u);
  EXPECT_EQ(c.valves[0].name, "A");
  EXPECT_EQ(c.valves[0].description, "Furnace to bone");
  EXPECT_EQ(c.valves[0].actuator, "actuator1");
  EXPECT_EQ(c.valves[0].address, "1");
  EXPECT_EQ(c.valves[0].interlocks, (std::vector<std::string>{"B"}));
  EXPECT_TRUE(c.valves[0].positive_interlocks.empty());
  EXPECT_EQ(c.valves[0].settle_ms, 1000);
  EXPECT_EQ(c.valves[1].settle_ms, 0);

  ASSERT_EQ(c.manual_valves.size(), 1u);
  EXPECT_EQ(c.manual_valves[0].name, "M1");

  ASSERT_EQ(c.switches.size(), 1u);
  EXPECT_EQ(c.switches[0].name, "pump_power");
  EXPECT_EQ(c.switches[0].actuator, "actuator1");
  EXPECT_EQ(c.switches[0].address, "9");
  EXPECT_EQ(c.switches[0].settle_ms, 0);

  ASSERT_EQ(c.gauges.size(), 1u);
  EXPECT_EQ(c.gauges[0].driver, "ig_controller");
  EXPECT_EQ(c.gauges[0].channel, 1);
  EXPECT_EQ(c.gauges[0].units, PressureUnits::Torr);
  ASSERT_TRUE(c.gauges[0].alarm_high);
  EXPECT_DOUBLE_EQ(*c.gauges[0].alarm_high, 1e-4);
  EXPECT_FALSE(c.gauges[0].alarm_low);

  ASSERT_EQ(c.pipettes.size(), 1u);
  EXPECT_EQ(c.pipettes[0].inner, "P1");
}

TEST(ConfigLoader, RecordsSourceLocations) {
  const auto text = example_with_pipette_valves();
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_TRUE(r);
  const auto& a = r->valves[0];
  EXPECT_EQ(a.path, "valves[0]");
  EXPECT_EQ(a.loc.file, "f.toml");
  EXPECT_EQ(a.loc.line, line_of(text, "[[valves]]"));
  EXPECT_EQ(a.where("actuator").line, line_of(text, "actuator = \"actuator1\""));
}

// The example as printed in the spec names pipette valves P1/P2 that it never
// declares; the validator must catch exactly that.
TEST(ConfigLoader, SpecExampleVerbatimReportsDanglingPipetteValves) {
  const auto text = std::string(test::kExampleConfig);
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_FALSE(rep.ok());
  EXPECT_EQ(rep.diagnostics.size(), 2u) << testing::PrintToString(test::formatted(rep.diagnostics));
  EXPECT_TRUE(has(rep.diagnostics, at(text, "inner = \"P1\"", "pipettes[0].inner: unknown valve 'P1'")));
  EXPECT_TRUE(has(rep.diagnostics, at(text, "outer = \"P2\"", "pipettes[0].outer: unknown valve 'P2'")));
}

TEST(ConfigLoader, SyntaxErrorCarriesLine) {
  const std::string text = "[system]\nname = \"x\"\nscan_interval_ms = = 3\n";
  auto rep = load_report_from_string(text, "f.toml");
  ASSERT_EQ(rep.diagnostics.size(), 1u);
  EXPECT_EQ(rep.diagnostics[0].loc.line, 3u);
  EXPECT_EQ(rep.diagnostics[0].field, "toml");
  EXPECT_FALSE(rep.ok());
}

TEST(ConfigLoader, MissingRequiredFieldPointsAtTable) {
  const std::string text = std::string(test::kPreamble) + "[[valves]]\nname = \"A\"\nactuator = \"act\"\n";
  auto rep = load_report_from_string(text, "f.toml");
  ASSERT_EQ(rep.diagnostics.size(), 1u);
  EXPECT_EQ(test::formatted(rep.diagnostics)[0], "f.toml:12:valves[0].address: missing required field");
}

TEST(ConfigLoader, MissingSystemTable) {
  auto rep = load_report_from_string("[transports.t]\nkind = \"sim\"\n", "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:1:system: missing required table [system]"));
}

TEST(ConfigLoader, WrongTypePointsAtValue) {
  const std::string text = std::string(test::kPreamble) +
                           "[[valves]]\nname = \"A\"\nactuator = \"act\"\naddress = \"1\"\nsettle_ms = \"slow\"\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:16:valves[0].settle_ms: expected integer, got string"));
}

TEST(ConfigLoader, WrongTypeInsideArray) {
  const std::string text = std::string(test::kPreamble) +
                           "[[valves]]\nname = \"A\"\nactuator = \"act\"\naddress = \"1\"\ninterlocks = [\"B\", 3]\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:16:valves[0].interlocks[1]: expected string, got integer"));
}

TEST(ConfigLoader, InvalidEnumValues) {
  const std::string text =
      "[system]\nname = \"x\"\n[transports.t]\nkind = \"usb\"\n"
      "[transports.s]\nkind = \"serial\"\nport = \"COM4\"\nparity = \"mark\"\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics,
                  "f.toml:4:transports.t.kind: invalid value 'usb' (expected serial | tcp | modbus_rtu | modbus_tcp | sim)"));
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:8:transports.s.parity: invalid value 'mark' (expected none | even | odd)"));
}

TEST(ConfigLoader, InvalidGaugeUnits) {
  const std::string text = std::string(test::kPreamble) + "[[gauges]]\nname = \"G\"\ndriver = \"gc\"\nunits = \"psi\"\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:15:gauges[0].units: invalid value 'psi' (expected torr | mbar | pa)"));
}

TEST(ConfigLoader, UnknownFieldsAreRejected) {
  const std::string text = std::string(test::kPreamble) +
                           "[[valves]]\nname = \"A\"\nacutator = \"act\"\nactuator = \"act\"\naddress = \"1\"\n"
                           "[extras]\nx = 1\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:14:valves[0].acutator: unknown field"));
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:17:extras: unknown field"));
}

TEST(ConfigLoader, TransportKeysDependOnKind) {
  const std::string text = "[system]\nname = \"x\"\n[transports.t]\nkind = \"tcp\"\nhost = \"h\"\nport = 1\nbaud = 9600\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:7:transports.t.baud: unknown field"));
}

TEST(ConfigLoader, RangeChecks) {
  const std::string text =
      "[system]\nname = \"x\"\nscan_interval_ms = 0\n"
      "[transports.t]\nkind = \"tcp\"\nhost = \"h\"\nport = 70000\n"
      "[transports.s]\nkind = \"serial\"\nport = \"COM1\"\nbaud = 0\ntimeout_ms = 0\n";
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:3:system.scan_interval_ms: value 0 out of range, must be >= 1"));
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:7:transports.t.port: value 70000 out of range, must be in [1, 65535]"));
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:11:transports.s.baud: value 0 out of range, must be >= 1"));
  EXPECT_TRUE(has(rep.diagnostics, "f.toml:12:transports.s.timeout_ms: value 0 out of range, must be >= 1"));
}

TEST(ConfigLoader, TransportDefaults) {
  const std::string text =
      "[system]\nname = \"x\"\n"
      "[transports.m]\nkind = \"modbus_tcp\"\nhost = \"10.0.0.2\"\n"
      "[transports.r]\nkind = \"modbus_rtu\"\nport = \"COM3\"\nparity = \"even\"\n"
      "[transports.s]\nkind = \"sim\"\n";
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(std::get<ModbusTcpParams>(r->transports.at("m").params).tcp.port, 502);
  const auto& rtu = std::get<ModbusRtuParams>(r->transports.at("r").params).serial;
  EXPECT_EQ(rtu.baud, 9600);
  EXPECT_EQ(rtu.data_bits, 8);
  EXPECT_EQ(rtu.stop_bits, 1);
  EXPECT_EQ(rtu.parity, Parity::Even);
  EXPECT_EQ(r->transports.at("s").kind, TransportKind::Sim);
  EXPECT_EQ(r->transports.at("s").timeout_ms, 500);
  EXPECT_EQ(r->transports.at("s").retries, 0);
}

TEST(ConfigLoader, IntegerAddressIsAccepted) {
  const std::string text = std::string(test::kPreamble) + "[[valves]]\nname = \"A\"\nactuator = \"act\"\naddress = 7\n";
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->valves[0].address, "7");
}

TEST(ConfigLoader, ReportsEveryErrorAndIsAllOrNothing) {
  const std::string text = std::string(test::kPreamble) +
                           "[[valves]]\nname = \"A\"\n"             // missing actuator + address
                           "[[gauges]]\nname = \"G\"\nchannel = \"one\"\n";  // missing driver, bad channel type
  auto rep = load_report_from_string(text, "f.toml");
  EXPECT_FALSE(rep.config.has_value());
  EXPECT_EQ(rep.diagnostics.size(), 4u) << testing::PrintToString(test::formatted(rep.diagnostics));

  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("f.toml:12:valves[0].actuator: missing required field"), std::string::npos);
  EXPECT_NE(r.error().what.find("f.toml:16:gauges[0].channel: expected integer, got string"), std::string::npos);
  EXPECT_EQ(std::count(r.error().what.begin(), r.error().what.end(), '\n'), 3);
}

TEST(ConfigLoader, LoadsFromFile) {
  const auto dir = std::filesystem::temp_directory_path() / "pychron_core_loader_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "extraction_line.toml";
  {
    std::ofstream(path) << example_with_pipette_valves();
  }
  auto r = load_system_config(path);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->source_file, path.generic_string());
  EXPECT_EQ(r->valves[0].loc.file, path.generic_string());
  std::filesystem::remove_all(dir);
}

TEST(ConfigLoader, MissingFileIsConfigError) {
  auto r = load_system_config(std::filesystem::path("/nonexistent/dir/extraction_line.toml"));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("cannot read config file"), std::string::npos);
}

TEST(Logging, ParsesAllKeys) {
  const std::string text = std::string(test::kPreamble) +
                           "[logging]\ndir = \"/var/log/pychron\"\nmax_size_mb = 20\nmax_files = 3\n"
                           "default_level = \"warn\"\necho_stderr = true\n"
                           "[logging.levels]\n\"transport.serial.*\" = \"trace\"\nscheduler = \"debug\"\n";
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_TRUE(r) << r.error().what;
  const auto& l = r->logging;
  EXPECT_EQ(l.dir, std::filesystem::path("/var/log/pychron"));
  EXPECT_EQ(l.max_size_mb, 20);
  EXPECT_EQ(l.max_files, 3);
  EXPECT_EQ(l.default_level, LogLevel::Warn);
  EXPECT_TRUE(l.echo_stderr);
  ASSERT_EQ(l.levels.size(), 2u);
  bool serial = false, sched = false;
  for (const auto& [glob, level] : l.levels) {
    if (glob == "transport.serial.*" && level == LogLevel::Trace) serial = true;
    if (glob == "scheduler" && level == LogLevel::Debug) sched = true;
  }
  EXPECT_TRUE(serial);
  EXPECT_TRUE(sched);
}

TEST(Logging, ExpandsHomeInDir) {
  const std::string text = std::string(test::kPreamble) + "[logging]\ndir = \"~/pychron-logs\"\n";
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_TRUE(r) << r.error().what;
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home != nullptr) {
    EXPECT_EQ(r->logging.dir, std::filesystem::path(home) / "pychron-logs");
  }
}

TEST(Logging, AbsentTableGivesDefaults) {
  auto r = load_system_config_from_string(std::string(test::kPreamble), "f.toml");
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->logging.max_size_mb, 10);
  EXPECT_EQ(r->logging.max_files, 5);
  EXPECT_EQ(r->logging.default_level, LogLevel::Info);
  EXPECT_TRUE(r->logging.dir.empty());
  EXPECT_TRUE(r->logging.levels.empty());
  EXPECT_FALSE(r->logging.echo_stderr);
}

TEST(Logging, BadLevelIsDiagnosticWithLine) {
  const std::string text = std::string(test::kPreamble) + "[logging]\ndefault_level = \"loud\"\n";
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("logging.default_level"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("f.toml:" + std::to_string(line_of(text, "default_level"))), std::string::npos)
      << r.error().what;
}

TEST(Logging, BadGlobLevelIsDiagnostic) {
  const std::string text = std::string(test::kPreamble) + "[logging.levels]\nscheduler = \"loud\"\nother = 3\n";
  auto r = load_system_config_from_string(text, "f.toml");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("logging.levels.scheduler"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("logging.levels.other"), std::string::npos) << r.error().what;
}

TEST(Logging, NonTableIsDiagnostic) {
  auto r = load_system_config_from_string("logging = 3\n" + std::string(test::kPreamble), "f.toml");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("logging"), std::string::npos) << r.error().what;
}

TEST(Logging, RejectsUnknownKeyAndNonPositiveSizes) {
  for (const char* body : {"bogus = 1", "max_files = 0", "max_size_mb = -1"}) {
    const std::string text = std::string(test::kPreamble) + "[logging]\n" + body + "\n";
    EXPECT_FALSE(load_system_config_from_string(text, "f.toml")) << body;
  }
}
