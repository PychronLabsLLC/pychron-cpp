#include "pychron/systems/spectrometer/bringup.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"

namespace pychron::spectrometer {

sim::BeamSettings beam_settings_from_config(const cfg::SpectrometerData& data) {
  sim::BeamSettings settings;
  if (data.config.source.nominal_hv) settings.nominal_hv = *data.config.source.nominal_hv;
  settings.table_value = [table = to_field_table(data.tables.at(data.config.magnet.field_table))](
                             double mass, const std::string& det) {
    auto v = table.value_for(mass, det);
    return v ? *v : mass / 8.0;
  };
  return settings;
}

namespace {

// Every driver libs/sim registers has a "sim_" kind (sim_drivers.cpp); the
// hardware driver for the same role is the kind without the prefix.
constexpr std::string_view kSimKindPrefix = "sim_";

// What makes the config reach hardware, transports first; nullopt when
// nothing does.
std::optional<std::string> first_non_simulated(const cfg::SpectrometerConfig& config) {
  for (const auto& [name, transport] : config.transports) {
    if (transport.kind != cfg::TransportKind::Sim) return "transport '" + name + "' is not kind \"sim\"";
  }
  for (const auto& [name, driver] : config.drivers) {
    if (!std::string_view(driver.kind).starts_with(kSimKindPrefix)) {
      return "driver '" + name + "' has kind \"" + driver.kind + "\", which is not a simulator";
    }
  }
  return std::nullopt;
}

}  // namespace

Result<void> feed_beam_from_line(sim::BeamModel& beam, sim::SimSystem& line) {
  // Everything is checked first: nothing below the checks can fail.
  if (&beam.clock() != &line.clock()) {
    return fail(ErrorKind::Config, "the simulated beam and the simulated line are on different clocks: a reading's "
                                   "instant would not be the line's time");
  }
  const sim::SimSettings& settings = line.settings();
  const std::string where = settings.file.empty() ? std::string("sim settings") : settings.file;
  const std::vector<std::string> known = beam.detector_names();
  for (const auto& [name, detector] : settings.detectors) {
    if (std::ranges::find(known, name) == known.end()) {
      std::string what = where + ": detectors." + name + ": unknown detector '" + name + "'; known: ";
      for (std::size_t i = 0; i < known.size(); ++i) what += (i == 0 ? "" : ", ") + known[i];
      if (known.empty()) what += "none";
      return fail(ErrorKind::Config, std::move(what));
    }
    if (!std::isfinite(detector.baseline) || !std::isfinite(detector.drift_per_h)) {
      return fail(ErrorKind::Config, where + ": detectors." + name + ": baseline and drift must be finite numbers");
    }
  }
  for (const auto& [name, detector] : settings.detectors) {
    (void)beam.set_baseline(name, detector.baseline, detector.drift_per_h);  // checked above
  }
  if (auto gas = line.beam_gas()) beam.set_gas_provider(std::move(gas));
  return {};
}

bool is_simulated(const cfg::SpectrometerData& data) { return !first_non_simulated(data.config).has_value(); }

Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(const std::filesystem::path& config,
                                                                SpectrometerContext ctx,
                                                                SpectrometerBringup options) {
  auto data = cfg::load_spectrometer(config);
  if (!data) return Unexpected(data.error());
  return load_spectrometer_for_app(std::move(*data), ctx, options);
}

Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(cfg::SpectrometerData data, SpectrometerContext ctx,
                                                                SpectrometerBringup options) {
  if (options.require_sim) {
    if (auto real = first_non_simulated(data.config)) {
      return fail(ErrorKind::Config,
                  "simulation requested but " + data.config.source_file + " is not a simulated spectrometer: " + *real);
    }
  }
  std::shared_ptr<sim::BeamModel> beam;
  if (options.sim_beam_from_table) {
    beam = std::make_shared<sim::BeamModel>(ctx.clock, beam_settings_from_config(data));
    sim::BeamModelRegistry::global().set("default", beam);
  }
  auto spec = SpectrometerAssembler::assemble(std::move(data), ctx);
  // The line's gas and baselines, now that the drivers have given the beam
  // its detectors.
  if (spec && beam && options.line_sim != nullptr) {
    if (auto fed = feed_beam_from_line(*beam, *options.line_sim); !fed) return fail(fed.error());
  }
  return spec;
}

}  // namespace pychron::spectrometer
