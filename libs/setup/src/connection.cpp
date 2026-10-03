#include "pychron/setup/connection.hpp"

#include <chrono>
#include <random>
#include <system_error>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

namespace pychron::setup {

namespace fs = std::filesystem;

namespace {

std::string describe(const spectrometer::cfg::SpectrometerConfig& c) {
  std::string out;
  for (const auto& [name, d] : c.drivers) {
    std::string where;
    if (auto t = c.transports.find(d.transport); t != c.transports.end()) {
      const auto& tc = t->second;
      if (tc.kind == spectrometer::cfg::TransportKind::Tcp || tc.kind == spectrometer::cfg::TransportKind::ModbusTcp) {
        where = " at " + tc.host + ":" + std::to_string(tc.tcp_port);
      } else if (!tc.serial_port.empty()) {
        where = " on " + tc.serial_port;
      }
    }
    out += (out.empty() ? "" : ", ") + name + ": " + d.kind + where;
  }
  return out;
}

// A scratch folder removed when this goes out of scope.
struct Scratch {
  fs::path dir;
  Scratch() {
    std::random_device rd;
    dir = fs::temp_directory_path() / ("pychron-connect-" + std::to_string(rd()) + std::to_string(rd()));
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

}  // namespace

Result<std::string> connect_spectrometer(const fs::path& spectrometer_toml) {
  auto data = spectrometer::cfg::load_spectrometer(spectrometer_toml);
  if (!data) return fail(ErrorKind::Config, to_string(data.error()));
  const std::string what = describe(data->config);
  if (spectrometer::is_simulated(*data)) return "simulated (" + what + "): nothing to connect to";
  // Assembling opens the transports and connects the drivers. The bus and
  // scheduler outlive the spectrometer (declared first, destroyed last).
  SteadyClock clock;
  SignalBus bus;
  Scheduler scheduler(clock, &bus, Scheduler::Options{0});
  auto spec = spectrometer::load_spectrometer_for_app(std::move(*data),
                                                      spectrometer::SpectrometerContext{clock, scheduler, bus});
  if (!spec) return fail(spec.error().kind, to_string(spec.error()));
  spec->reset();
  return "connected: " + what;
}

Result<std::string> test_instrument_connection(const ProfileLibrary& library, const ResolvedProfile& profile,
                                               const Answers& answers) {
  Scratch scratch;
  auto plan = plan_install(library, profile, answers, scratch.dir);
  if (!plan) return fail(std::move(plan).error());
  if (auto written = apply_install(*plan); !written) return fail(std::move(written).error());
  const fs::path config = scratch.dir / "spectrometer.toml";
  std::error_code ec;
  if (!fs::exists(config, ec)) return fail(ErrorKind::Config, "this profile installs no spectrometer.toml");
  return connect_spectrometer(config);
}

}  // namespace pychron::setup
