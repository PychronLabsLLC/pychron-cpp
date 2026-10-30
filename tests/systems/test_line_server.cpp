// LineServer: a line's config served on sockets (sim serve spec 4, 5). Real
// sockets on loopback, real time; nothing here waits on them.
#include "pychron/systems/line_server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "hardware_style.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/transport/tcp_transport.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
namespace fs = std::filesystem;

const fs::path kExamples = PYCHRON_EXAMPLE_CONFIGS_DIR;

std::string read_text(const fs::path& p) {
  std::ifstream in(p);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

config::SystemConfig example(bool on_the_wire) {
  std::string text = read_text(kExamples / "extraction_line.toml");
  if (on_the_wire) text = pychron::testing::hardware_style(text);
  auto cfg = config::load_system_config_from_string(text, (kExamples / "extraction_line.toml").string());
  EXPECT_TRUE(cfg) << cfg.error().what;
  return *cfg;
}

config::SystemConfig from_text(const std::string& text) {
  auto cfg = config::load_system_config_from_string(text, "t.toml");
  EXPECT_TRUE(cfg) << cfg.error().what;
  return *cfg;
}

LineServerOptions example_options() {
  LineServerOptions o;
  o.sim.sim_file = sim_file_beside(kExamples / "extraction_line.toml", true).read;
  return o;
}

bool has_note(const LineServer& server, LogLevel level, const std::string& text) {
  for (const auto& n : server.notes())
    if (n.level == level && n.message.find(text) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST(LineServerSteady, ServesHardwareStyleExample) {
  const SteadyClock clock;
  const auto cfg = example(true);
  auto canvas = canvas::load_canvas(kExamples / "canvas.toml");
  ASSERT_TRUE(canvas) << canvas.error().what;
  auto server = LineServer::start(cfg, &*canvas, clock, example_options());
  ASSERT_TRUE(server) << server.error().what;
  EXPECT_TRUE((*server)->in_process().empty());
  std::map<std::string, std::string> emulators;
  for (const auto& s : (*server)->served()) {
    EXPECT_NE(s.port, 0) << s.name;
    emulators[s.name] = s.emulator;
  }
  EXPECT_EQ(emulators, (std::map<std::string, std::string>{{"bakeout_plc", "plc2000_heater"},
                                                           {"cg_controller", "gp_microion"},
                                                           {"cryostat", "lakeshore"},
                                                           {"gauge_net", "pfeiffer_maxigauge"},
                                                           {"laser_pc", "chromium"},
                                                           {"valve_bus", "proxr_relay"}}));
  const auto endpoints = (*server)->endpoints();
  EXPECT_EQ(endpoints.config, fs::weakly_canonical(kExamples / "extraction_line.toml"));
  EXPECT_EQ(endpoints.host, "127.0.0.1");
  EXPECT_EQ(endpoints.transports.size(), 6U);
  EXPECT_EQ(endpoints.transports.at("bakeout_plc").protocol, WireProtocol::Tcp);
}

TEST(LineServerSteady, SimTransportsInProcess) {
  const SteadyClock clock;
  auto server = LineServer::start(example(false), nullptr, clock);
  ASSERT_TRUE(server) << server.error().what;
  EXPECT_TRUE((*server)->served().empty());
  EXPECT_EQ((*server)->in_process().size(), 6U);
}

TEST(LineServerSteady, SerialNoted) {
  const SteadyClock clock;
  auto server = LineServer::start(from_text(R"(
[system]
name = "t"
[transports.bus]
kind = "serial"
port = "/dev/ttyUSB0"
[drivers.relay]
kind = "proxr_relay"
transport = "bus"
)"),
                                  nullptr, clock);
  ASSERT_TRUE(server) << server.error().what;
  ASSERT_EQ((*server)->served().size(), 1U);
  EXPECT_EQ((*server)->served()[0].kind, "serial");
  EXPECT_EQ((*server)->served()[0].protocol, WireProtocol::Tcp);
  EXPECT_TRUE(has_note(**server, LogLevel::Info, "bus: serial served as TCP"));
}

TEST(LineServerSteady, UnknownKindWarns) {
  const SteadyClock clock;
  auto server = LineServer::start(from_text(R"(
[system]
name = "t"
[transports.box]
kind = "udp"
host = "192.0.2.1"
port = 9000
[drivers.thing]
kind = "mystery_box"
transport = "box"
)"),
                                  nullptr, clock);
  ASSERT_TRUE(server) << server.error().what;
  ASSERT_EQ((*server)->served().size(), 1U);
  EXPECT_EQ((*server)->served()[0].protocol, WireProtocol::Udp);
  EXPECT_EQ((*server)->served()[0].emulator, "");
  EXPECT_TRUE(has_note(**server, LogLevel::Warn, "box: no emulator for driver kind 'mystery_box': silent wire"));
}

TEST(LineServerSteady, SameHostPortRefusedNamingBoth) {
  const SteadyClock clock;
  auto server = LineServer::start(from_text(R"(
[system]
name = "t"
[transports.one]
kind = "tcp"
host = "192.0.2.1"
port = 7000
[transports.two]
kind = "modbus_tcp"
host = "192.0.2.1"
port = 7000
)"),
                                  nullptr, clock);
  ASSERT_FALSE(server);
  EXPECT_EQ(server.error().kind, ErrorKind::Config);
  EXPECT_NE(server.error().what.find("'one' and 'two'"), std::string::npos) << server.error().what;
}

TEST(LineServerSteady, EndpointsCarryAdvertise) {
  const SteadyClock clock;
  LineServerOptions options;
  options.advertise = "10.1.2.3";
  auto server = LineServer::start(example(true), nullptr, clock, options);
  ASSERT_TRUE(server) << server.error().what;
  const auto endpoints = (*server)->endpoints();
  EXPECT_EQ(endpoints.host, "10.1.2.3");
  for (const auto& [name, e] : endpoints.transports) EXPECT_EQ(e.host, "10.1.2.3") << name;
}

namespace {

std::optional<spectrometer::cfg::SpectrometerData> example_spectrometer(const std::string& file) {
  auto data = spectrometer::cfg::load_spectrometer(kExamples / file);
  EXPECT_TRUE(data) << data.error().what;
  if (!data) return std::nullopt;
  return std::move(*data);
}

const ServedTransport* served_named(const LineServer& server, const std::string& name, bool spectrometer) {
  for (const auto& s : server.served())
    if (s.name == name && s.spectrometer == spectrometer) return &s;
  return nullptr;
}

constexpr const char* kQtegraValves = R"(
[system]
name = "t"
[transports.ms_pc]
kind = "tcp"
host = "192.168.0.10"
port = 1069
[drivers.ms_valves]
kind = "qtegra_valves"
transport = "ms_pc"
[[valves]]
name = "V1"
actuator = "ms_valves"
address = "Valve 1"
)";

}  // namespace

TEST(LineServerSteady, ServesQtegraSpectrometer) {
  const SteadyClock clock;
  auto canvas = canvas::load_canvas(kExamples / "canvas.toml");
  ASSERT_TRUE(canvas);
  LineServerOptions options = example_options();
  options.spectrometer = example_spectrometer("spectrometer.qtegra.toml");
  ASSERT_TRUE(options.spectrometer);
  auto server = LineServer::start(example(true), &*canvas, clock, std::move(options));
  ASSERT_TRUE(server) << server.error().what;
  const auto* qtegra = served_named(**server, "qtegra", true);
  ASSERT_NE(qtegra, nullptr);
  EXPECT_EQ(qtegra->emulator, "thermo_qtegra");
  EXPECT_NE(qtegra->port, 0);
  ASSERT_NE((*server)->beam(), nullptr);
  EXPECT_TRUE((*server)->beam()->has_detector("H1"));
  const auto endpoints = (*server)->endpoints();
  EXPECT_EQ(endpoints.spectrometer, fs::weakly_canonical(kExamples / "spectrometer.qtegra.toml"));
  EXPECT_EQ(endpoints.spectrometer_transports.at("qtegra").port, qtegra->port);
  EXPECT_FALSE(has_note(**server, LogLevel::Warn, "not joined to the line"));
}

TEST(LineServerSteady, ServesNgxSpectrometer) {
  const SteadyClock clock;
  LineServerOptions options;
  options.spectrometer = example_spectrometer("spectrometer.ngx.toml");
  ASSERT_TRUE(options.spectrometer);
  auto server = LineServer::start(from_text("[system]\nname = \"t\"\n"), nullptr, clock, std::move(options));
  ASSERT_TRUE(server) << server.error().what;
  const auto* ngx = served_named(**server, "ngx", true);
  ASSERT_NE(ngx, nullptr);
  EXPECT_EQ(ngx->emulator, "isotopx_ngx");
  EXPECT_TRUE(has_note(**server, LogLevel::Warn, "not joined to the line"));
}

TEST(LineServerSteady, QtegraLineAndSpectrometerShareOnePort) {
  const SteadyClock clock;
  LineServerOptions options;
  options.spectrometer = example_spectrometer("spectrometer.qtegra.toml");
  ASSERT_TRUE(options.spectrometer);
  auto server = LineServer::start(from_text(kQtegraValves), nullptr, clock, std::move(options));
  ASSERT_TRUE(server) << server.error().what;
  const auto* line = served_named(**server, "ms_pc", false);
  const auto* spec = served_named(**server, "qtegra", true);
  ASSERT_NE(line, nullptr);
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(line->port, spec->port);
  EXPECT_EQ(spec->shares, "ms_pc");
  EXPECT_EQ(line->emulator, "qtegra_valves+thermo_qtegra");
}

TEST(LineServerSteady, LinkTransportJoinsNgx) {
  const SteadyClock clock;
  LineServerOptions options;
  options.spectrometer = example_spectrometer("spectrometer.ngx.toml");
  ASSERT_TRUE(options.spectrometer);
  auto server = LineServer::start(from_text(R"(
[system]
name = "t"
[transports.ngx_link]
kind = "link"
link = "ngx"
[drivers.ngx_valves]
kind = "ngx_valves"
transport = "ngx_link"
)"),
                                  nullptr, clock, std::move(options));
  ASSERT_TRUE(server) << server.error().what;
  const auto* ngx = served_named(**server, "ngx", true);
  ASSERT_NE(ngx, nullptr);
  EXPECT_EQ(ngx->emulator, "isotopx_ngx+ngx_valves");
  EXPECT_EQ(served_named(**server, "ngx_link", false), nullptr);  // borrows, is not served
}

TEST(LineServerSteady, MixedVendorsOnOneAddressRefused) {
  const SteadyClock clock;
  LineServerOptions options;
  options.spectrometer = example_spectrometer("spectrometer.qtegra.toml");
  ASSERT_TRUE(options.spectrometer);
  auto server = LineServer::start(from_text(R"(
[system]
name = "t"
[transports.relays]
kind = "tcp"
host = "192.168.0.10"
port = 1069
[drivers.relay]
kind = "proxr_relay"
transport = "relays"
)"),
                                  nullptr, clock, std::move(options));
  ASSERT_FALSE(server);
  EXPECT_EQ(server.error().kind, ErrorKind::Config);
  EXPECT_NE(server.error().what.find("'relays' and 'qtegra (spectrometer)'"), std::string::npos)
      << server.error().what;
}

TEST(LineServerSteady, NgxBannerOnEveryConnection) {
  const SteadyClock clock;
  LineServerOptions options;
  options.spectrometer = example_spectrometer("spectrometer.ngx.toml");
  ASSERT_TRUE(options.spectrometer);
  auto server = LineServer::start(from_text("[system]\nname = \"t\"\n"), nullptr, clock, std::move(options));
  ASSERT_TRUE(server) << server.error().what;
  const auto port = served_named(**server, "ngx", true)->port;
  for (int i = 0; i < 2; ++i) {
    TransportOptions o;
    o.name = "client";
    o.timeout = std::chrono::seconds(5);
    TcpTransport client({"127.0.0.1", port}, o);
    ASSERT_TRUE(client.open()) << i;
    auto banner = client.read(ReadSpec::until("\n"));
    ASSERT_TRUE(banner) << i << ": " << banner.error().what;
    EXPECT_NE(to_string(*banner).find("NGX"), std::string::npos) << to_string(*banner);
  }
}
