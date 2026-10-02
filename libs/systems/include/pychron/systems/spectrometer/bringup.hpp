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
};

// BeamSettings whose table_value follows the config's active field table
// (mass / 8 where the table has no value) and nominal_hv from the config.
sim::BeamSettings beam_settings_from_config(const cfg::SpectrometerData& data);

// Errors from loading or assembling are returned unchanged.
Result<std::unique_ptr<Spectrometer>> load_spectrometer_for_app(const std::filesystem::path& config,
                                                                SpectrometerContext ctx,
                                                                SpectrometerBringup options = {});

}  // namespace pychron::spectrometer
