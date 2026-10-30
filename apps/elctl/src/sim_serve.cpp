#include "sim_serve.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <string_view>
#include <system_error>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/line_server.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/transport/endpoints.hpp"
#include "exp.hpp"

namespace elctl {

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace pychron;

constexpr const char* kServeUsage =
    "usage: elctl sim serve [--config <extraction_line.toml>] [--canvas <canvas.toml>]\n"
    "                       [--spectrometer <spectrometer.toml> | --no-spectrometer]\n"
    "                       [--bind <addr>] [--advertise <host>] [--port-base <n>] [--endpoints <file>]\n";

int usage(Io io, const std::string& message) {
  io.err << "elctl sim serve: " << message << '\n' << kServeUsage;
  return kUsage;
}

// 127.0.0.0/8, ::1 and localhost: what only this machine reaches.
bool is_loopback(std::string_view address) {
  return address.starts_with("127.") || address == "::1" || address == "localhost";
}

const char* protocol_name(WireProtocol p) { return p == WireProtocol::Udp ? "udp" : "tcp"; }

void print_table(const systems::LineServer& server, std::ostream& out) {
  out << std::left << std::setw(22) << "transport" << std::setw(10) << "protocol" << std::setw(7) << "port"
      << "emulator\n";
  for (const auto& s : server.served()) {
    out << std::setw(22) << (s.spectrometer ? s.name + " (spectrometer)" : s.name) << std::setw(10)
        << protocol_name(s.protocol) << std::setw(7) << s.port << (s.emulator.empty() ? "(silent)" : s.emulator);
    if (!s.shares.empty()) out << ", shares " << s.shares;
    out << '\n';
  }
  for (const auto& name : server.in_process()) {
    out << std::setw(22) << name << std::setw(10) << "-" << std::setw(7) << "-" << "in-process, not joined\n";
  }
  out << std::right;
  for (const auto& note : server.notes()) {
    out << (note.level >= LogLevel::Warn ? "warning: " : "note: ") << note.message << '\n';
  }
}

}  // namespace

int sim_serve_command(const std::vector<std::string>& args, const fs::path& line_file, const fs::path& install_canvas,
                      const fs::path& install_spectrometer, Io io, const std::function<bool()>& stop) {
  fs::path line = line_file;
  std::optional<fs::path> canvas_file;
  if (!install_canvas.empty()) canvas_file = install_canvas;
  std::optional<fs::path> endpoints_file;
  std::optional<fs::path> spectrometer_file;
  {
    std::error_code ec;
    if (!install_spectrometer.empty() && fs::is_regular_file(install_spectrometer, ec))
      spectrometer_file = install_spectrometer;
  }
  systems::LineServerOptions options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const bool has_value = i + 1 < args.size();
    if (a == "--no-spectrometer") {
      spectrometer_file.reset();
      continue;
    }
    if (a == "--config" || a == "--canvas" || a == "--spectrometer" || a == "--bind" || a == "--advertise" || a == "--port-base" ||
        a == "--endpoints") {
      if (!has_value) return usage(io, a + " needs a value");
      const std::string& v = args[++i];
      if (a == "--config") {
        line = v;
      } else if (a == "--canvas") {
        canvas_file = v;
      } else if (a == "--spectrometer") {
        spectrometer_file = v;
      } else if (a == "--bind") {
        options.bind = v;
      } else if (a == "--advertise") {
        options.advertise = v;
      } else if (a == "--endpoints") {
        endpoints_file = v;
      } else {
        int port = 0;
        const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), port);
        if (ec != std::errc{} || end != v.data() + v.size()) port = 0;
        if (port < 1 || port > 65535) return usage(io, "--port-base needs a port, 1 to 65535");
        options.port_base = static_cast<std::uint16_t>(port);
      }
    } else {
      return usage(io, "unknown option '" + a + "'");
    }
  }

  auto config = config::load_system_config(line);
  if (!config) {
    io.err << "error: " << to_string(config.error()) << '\n';
    return kFailed;
  }
  if (!canvas_file) {
    const auto beside = line.parent_path() / "canvas.toml";
    std::error_code ec;
    if (fs::is_regular_file(beside, ec)) canvas_file = beside;
  }
  std::optional<canvas::Canvas> canvas_model;
  if (canvas_file) {
    auto loaded = canvas::load_canvas(*canvas_file);
    if (!loaded) {
      io.err << "error: " << to_string(loaded.error()) << '\n';
      return kFailed;
    }
    canvas_model = std::move(*loaded);
  }
  if (config->sim.file.empty()) {
    auto beside = systems::sim_file_beside(line, canvas_model.has_value());
    options.sim.sim_file = std::move(beside.read);
    options.sim.sim_beside_skipped = std::move(beside.skipped);
  }
  if (spectrometer_file) {
    auto tables = spectrometer_table_root(*spectrometer_file);
    if (!tables) {
      io.err << "error: " << to_string(tables.error()) << '\n';
      return kFailed;
    }
    auto data = spectrometer::cfg::load_spectrometer(*spectrometer_file, *tables);
    if (!data) {
      io.err << "error: " << to_string(data.error()) << '\n';
      return kFailed;
    }
    options.spectrometer = std::move(*data);
  }
  if (!is_loopback(options.bind)) {
    io.err << "warning: serving on " << options.bind
           << ": the emulators accept any login and have no authentication; anyone who can reach this address can "
              "drive the simulated lab\n";
  }

  const SteadyClock clock;
  auto server = systems::LineServer::start(*config, canvas_model ? &*canvas_model : nullptr, clock, options);
  if (!server) {
    io.err << "error: " << to_string(server.error()) << '\n';
    return kFailed;
  }
  const fs::path where = endpoints_file ? *endpoints_file : default_sim_endpoints_path(line);
  if (auto written = write_sim_endpoints(where, (*server)->endpoints()); !written) {
    io.err << "error: " << to_string(written.error()) << '\n';
    return kFailed;
  }
  print_table(**server, io.out);
  io.out << "endpoints: " << where.generic_string() << "\nserving; interrupt to stop\n" << std::flush;

  while (!stop()) clock.sleep_for(100ms);

  (*server)->stop();
  std::error_code ec;
  fs::remove(where, ec);
  io.out << "stopped\n";
  return kOk;
}

}  // namespace elctl
