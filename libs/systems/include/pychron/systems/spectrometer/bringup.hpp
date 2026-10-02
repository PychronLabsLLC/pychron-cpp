#pragma once

// App bring-up: load a spectrometer config and, in simulation, register a
// beam model whose peaks sit where the config's field table says, so a UI or
// UI test gets a working sim spectrometer without hand-building the beam.

#include <filesystem>
#include <memory>

#include "pychron/core/error.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::spectrometer {

struct SpectrometerBringup {
  // Register a BeamModel following the config's field table as "default"
  // before assembling. Leave false for real hardware or a caller-built beam.
  bool sim_beam_from_table = false;
  // Refuse a config that is not simulated (see is_simulated) before anything
  // is assembled, any transport opened or the beam registry touched.
  bool require_sim = false;
};

// True when nothing in the config can reach hardware: every declared
// transport has kind sim (or none is declared) and every driver's kind is a
// simulator kind, i.e. carries the "sim_" prefix all libs/sim drivers register
// under.
bool is_simulated(const cfg::SpectrometerData& data);

// BeamSettings whose table_value follows the config's active field table
// (mass / 8 where the table has no value) and nominal_hv from the config.
sim::BeamSettings beam_settings_from_config(const cfg::SpectrometerData& data);

// Errors from loading or assembling are returned unchanged. With require_sim,
// a config that is not simulated is a Config error naming the first
// non-simulated transport or driver.
Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(const std::filesystem::path& config,
                                                                SpectrometerContext ctx,
                                                                SpectrometerBringup options = {});
// The same for a config already loaded, so a caller can inspect it (e.g.
// is_simulated) before choosing the options.
Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(cfg::SpectrometerData data, SpectrometerContext ctx,
                                                                SpectrometerBringup options = {});

}  // namespace pychron::spectrometer
