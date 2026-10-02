#include "pychron/systems/spectrometer/bringup.hpp"

#include <string>
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

Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(const std::filesystem::path& config,
                                                                SpectrometerContext ctx,
                                                                SpectrometerBringup options) {
  auto data = cfg::load_spectrometer(config);
  if (!data) return Unexpected(data.error());
  if (options.sim_beam_from_table) {
    sim::BeamModelRegistry::global().set(
        "default", std::make_shared<sim::BeamModel>(ctx.clock, beam_settings_from_config(*data)));
  }
  return SpectrometerAssembler::assemble(std::move(*data), ctx);
}

}  // namespace pychron::spectrometer
