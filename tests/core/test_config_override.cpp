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
  EXPECT_TRUE(has(rep.diagnostics, "main.local.toml:1:system: local override may only set [transports.<name>] keys"));
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
