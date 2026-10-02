#include "pychron/systems/spectrometer/bringup.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
  if (options.sim_beam_from_table) {
    sim::BeamModelRegistry::global().set(
        "default", std::make_shared<sim::BeamModel>(ctx.clock, beam_settings_from_config(data)));
  }
  return SpectrometerAssembler::assemble(std::move(data), ctx);
}

}  // namespace pychron::spectrometer
