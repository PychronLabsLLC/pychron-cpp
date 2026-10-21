#pragma once

// Shared set-up for the spectrometer UI suites: the sim-integrated example
// spectrometer with a started scheduler and a beam whose peaks follow the
// config's field table. Nothing scans until a test starts it. It runs on a
// real clock unless a test hands it another one (a VirtualClock, say), which
// must outlive it.

#include <filesystem>
#include <functional>
#include <memory>

#include <QtTest/QtTest>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"

namespace pychron::ui::test {

// Destroy any bridge or window built on this before the fixture itself.
struct SimSpectrometer {
  SteadyClock steady;
  const Clock& clock;  // `steady` unless the maker was given one
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{2}};
  std::unique_ptr<spectrometer::Spectrometer> spec;
  std::unique_ptr<spectrometer::ScanService> scan;

  explicit SimSpectrometer(const Clock* on = nullptr) : clock(on != nullptr ? *on : steady) {}
  SimSpectrometer(const SimSpectrometer&) = delete;
  SimSpectrometer& operator=(const SimSpectrometer&) = delete;

  // The scan stops while the scheduler still runs its jobs; the scheduler is
  // idle before the spectrometer goes (stop() only joins the dispatcher, so a
  // poll already on a worker is waited for); the global beam registry refers
  // to `clock`, so it is emptied before the members are destroyed.
  ~SimSpectrometer() {
    scan.reset();
    scheduler.stop();
    scheduler.wait_idle();
    spec.reset();
    sim::BeamModelRegistry::global().clear();
  }
};

// `edit`, when given, changes the loaded config before the spectrometer is
// built from it (a detector's colour, say). `clock`, when given, is the one
// everything runs on instead of the fixture's own SteadyClock.
inline std::unique_ptr<SimSpectrometer> make_sim_spectrometer(
    const std::function<void(spectrometer::cfg::SpectrometerData&)>& edit = {}, const Clock* clock = nullptr) {
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  sim::BeamModelRegistry::global().clear();
  auto data = spectrometer::cfg::load_spectrometer(dir / "spectrometer.sim-integrated.toml");
  if (!data) {
    qFatal("cannot load sim spectrometer config: %s", to_string(data.error()).c_str());
  }
  if (edit) edit(*data);
  auto sim = std::make_unique<SimSpectrometer>(clock);
  sim->scheduler.start();
  auto spec = spectrometer::load_spectrometer_for_app(
      std::move(*data), spectrometer::SpectrometerContext{sim->clock, sim->scheduler, sim->bus},
      spectrometer::SpectrometerBringup{.sim_beam_from_table = true});
  if (!spec) {
    qFatal("cannot load sim spectrometer: %s", to_string(spec.error()).c_str());
  }
  sim->spec = std::move(*spec);
  sim->scan = std::make_unique<spectrometer::ScanService>(*sim->spec, sim->bus, sim->clock);
  return sim;
}

}  // namespace pychron::ui::test
