#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "config_fixtures.hpp"
#include "pychron/core/config/loader.hpp"

using namespace pychron;
using namespace pychron::config;
using pychron::test::has;

namespace {

constexpr std::string_view kMain = R"toml([system]
name = "jan"
[transports.valve_bus]
kind = "serial"
port = "/dev/tty.usbserial-A1"
baud = 9600
retries = 2
[transports.gauge_net]
kind = "tcp"
host = "192.168.0.51"
port = 8000
)toml";

LoadReport load(std::string_view local) {
  return load_report_from_string(kMain, "main.toml", local, "main.local.toml");
}

}  // namespace

TEST(ConfigOverride, LocalFileSetsPortHostBaud) {
  auto rep = load(
      "[transports.valve_bus]\nport = \"COM4\"\nbaud = 19200\n"
      "[transports.gauge_net]\nhost = \"127.0.0.1\"\n");
  ASSERT_TRUE(rep.ok()) << testing::PrintToString(test::formatted(rep.diagnostics));
  const auto& vb = rep.config->transports.at("valve_bus");
  EXPECT_EQ(std::get<SerialParams>(vb.params).port, "COM4");
  EXPECT_EQ(std::get<SerialParams>(vb.params).baud, 19200);
  EXPECT_EQ(vb.retries, 2);  // untouched keys come from the main file
  EXPECT_EQ(vb.where("port").file, "main.local.toml");
  EXPECT_EQ(vb.where("port").line, 2u);
  EXPECT_EQ(vb.where("retries").file, "main.toml");

  const auto& gn = rep.config->transports.at("gauge_net");
  EXPECT_EQ(std::get<TcpParams>(gn.params).host, "127.0.0.1");
  EXPECT_EQ(std::get<TcpParams>(gn.params).port, 8000);
}

TEST(ConfigOverride, NonTransportKeysAreRejected) {
  auto rep = load("[transports.valve_bus]\nretries = 9\nkind = \"sim\"\n");
  EXPECT_FALSE(rep.ok());
  EXPECT_TRUE(has(rep.diagnostics,
                  "main.local.toml:2:transports.valve_bus.retries: key may not be overridden locally "
                  "(allowed: port, host, baud, data_bits, stop_bits, parity)"));
  EXPECT_TRUE(has(rep.diagnostics,
                  "main.local.toml:3:transports.valve_bus.kind: key may not be overridden locally "
                  "(allowed: port, host, baud, data_bits, stop_bits, parity)"));
}

TEST(ConfigOverride, OnlyTransportsSectionAllowed) {
  auto rep = load("[system]\nname = \"other\"\n");
  EXPECT_TRUE(has(rep.diagnostics, "main.local.toml:1:system: local override may only set [transports.<name>] keys, [logging] and [metrics]"));
}

TEST(ConfigOverride, UnknownTransportRejected) {
  auto rep = load("[transports.nope]\nport = \"COM1\"\n");
  EXPECT_TRUE(has(rep.diagnostics, "main.local.toml:1:transports.nope: local override targets unknown transport 'nope'"));
}

TEST(ConfigOverride, KeyMustSuitTransportKind) {
  auto rep = load("[transports.valve_bus]\nhost = \"10.0.0.1\"\n");
  EXPECT_TRUE(has(rep.diagnostics, "main.local.toml:2:transports.valve_bus.host: unknown field for this transport kind"));
}

TEST(ConfigOverride, TypeErrorsInOverridePointAtLocalFile) {
  auto rep = load("[transports.valve_bus]\nbaud = \"fast\"\n");
  EXPECT_TRUE(has(rep.diagnostics, "main.local.toml:2:transports.valve_bus.baud: expected integer, got string"));
}

TEST(ConfigOverride, OverridePathIsSibling) {
  EXPECT_EQ(local_override_path("/etc/lab/extraction_line.toml"),
            std::filesystem::path("/etc/lab/extraction_line.local.toml"));
}

TEST(ConfigOverride, FileLoaderPicksUpLocalOverride) {
  const auto dir = std::filesystem::temp_directory_path() / "pychron_core_override_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto main = dir / "extraction_line.toml";
  { std::ofstream(main) << kMain; }

  auto without = load_system_config(main);
  ASSERT_TRUE(without) << without.error().what;
  EXPECT_EQ(std::get<SerialParams>(without->transports.at("valve_bus").params).port, "/dev/tty.usbserial-A1");

  { std::ofstream(dir / "extraction_line.local.toml") << "[transports.valve_bus]\nport = \"COM7\"\n"; }
  auto with = load_system_config(main);
  ASSERT_TRUE(with) << with.error().what;
  EXPECT_EQ(std::get<SerialParams>(with->transports.at("valve_bus").params).port, "COM7");

  std::filesystem::remove_all(dir);
}

// ---- [logging] and [metrics] in the local file -------------------------------
// What File > Preferences writes: this computer's logging and its metrics
// endpoint, over what the shared file says.

namespace {

constexpr std::string_view kMainWithLogging = R"toml([system]
name = "jan"
[logging]
dir = "/var/log/pychron"
max_files = 3
default_level = "warn"
[logging.levels]
scheduler = "debug"
"*.wire" = "trace"
[metrics]
enabled = true
port = 9500
)toml";

LoadReport load_over_logging(std::string_view local) {
  return load_report_from_string(kMainWithLogging, "main.toml", local, "main.local.toml");
}

// A diagnostic for `field` whose message has `part` in it.
bool mentions(const std::vector<Diagnostic>& ds, std::string_view field, std::string_view part) {
  for (const Diagnostic& d : ds) {
    if (d.field == field && d.message.find(part) != std::string::npos) return true;
  }
  return false;
}

LogLevel level_of(const LoggingConfig& l, std::string_view glob) {
  for (const auto& [g, level] : l.levels) {
    if (g == glob) return level;
  }
  ADD_FAILURE() << "no level for " << glob;
  return LogLevel::Info;
}

}  // namespace

TEST(ConfigOverride, LocalLoggingKeysWinAndTheRestStay) {
  const auto r = load_over_logging("[logging]\ndefault_level = \"trace\"\nmax_size_mb = 50\n");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.config->logging.default_level, LogLevel::Trace);
  EXPECT_EQ(r.config->logging.max_size_mb, 50);
  EXPECT_EQ(r.config->logging.max_files, 3);  // the shared file's
  EXPECT_EQ(r.config->logging.dir, std::filesystem::path("/var/log/pychron"));
  EXPECT_EQ(r.config->logging.levels.size(), 2u);
}

// The local table is the whole list: a level the shared file sets can be
// dropped here, which a merge could not express.
TEST(ConfigOverride, LocalLevelsReplaceTheSharedOnes) {
  const auto r = load_over_logging("[logging.levels]\nscheduler = \"error\"\nmetrics = \"debug\"\n");
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r.config->logging.levels.size(), 2u);
  EXPECT_EQ(level_of(r.config->logging, "scheduler"), LogLevel::Error);
  EXPECT_EQ(level_of(r.config->logging, "metrics"), LogLevel::Debug);
  EXPECT_EQ(r.config->logging.default_level, LogLevel::Warn);
}

TEST(ConfigOverride, AnEmptyLocalLevelsTableClearsThem) {
  const auto r = load_over_logging("[logging.levels]\n");
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.config->logging.levels.empty());
}

TEST(ConfigOverride, LocalLoggingWithoutASharedTable) {
  const auto r = load("[logging]\ndefault_level = \"debug\"\necho_stderr = true\n");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.config->logging.default_level, LogLevel::Debug);
  EXPECT_TRUE(r.config->logging.echo_stderr);
}

TEST(ConfigOverride, LocalMetricsKeysWinAndTheRestStay) {
  const auto r = load_over_logging("[metrics]\nbind = \"127.0.0.1\"\n");
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.config->metrics.enabled);  // the shared file's
  EXPECT_EQ(r.config->metrics.port, 9500);
  EXPECT_EQ(r.config->metrics.bind, "127.0.0.1");
}

TEST(ConfigOverride, LocalMetricsCanTurnTheEndpointOff) {
  const auto r = load_over_logging("[metrics]\nenabled = false\n");
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.config->metrics.enabled);
}

TEST(ConfigOverride, ABadLocalLoggingOrMetricsValuePointsAtTheLocalFile) {
  const auto r = load_over_logging("[logging]\ndefault_level = \"loud\"\n[metrics]\nport = 0\nbind = \"labpc\"\n");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(mentions(r.diagnostics, "logging.default_level", "invalid value"));
  EXPECT_TRUE(mentions(r.diagnostics, "metrics.port", "out of range"));
  EXPECT_TRUE(mentions(r.diagnostics, "metrics.bind", "not an IP address"));
  for (const auto& d : r.diagnostics) EXPECT_EQ(d.loc.file, "main.local.toml") << d.field;
}

TEST(ConfigOverride, AnUnknownLocalLoggingOrMetricsKeyIsRefused) {
  const auto r = load_over_logging("[logging]\ncolour = true\n[metrics]\ntls = true\n");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.diagnostics.size(), 2u);
}

TEST(ConfigOverride, OtherTablesAreStillRefusedInTheLocalFile) {
  const auto r = load_over_logging("[system]\nname = \"other\"\n[metrics]\nenabled = false\n");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(mentions(r.diagnostics, "system", "local override may only set"));
}
