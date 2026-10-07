#pragma once

// App bring-up: load a spectrometer config and, in simulation, register a
// beam model whose peaks sit where the config's field table says, so a UI or
// UI test gets a working sim spectrometer without hand-building the beam.
//
// Where a simulated extraction line is there too, the beam measures what the
// line's source volume holds (lab simulator spec section 5.2): give the
// line's `SimSystem` as `SpectrometerBringup::line_sim`, or, with a beam
// built by hand, call `feed_beam_from_line`. A spectrometer brought up with
// no line reads the beam's fixed argon, as it always did.

#include <filesystem>
#include <memory>

#include "pychron/core/error.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::sim {
class SimSystem;
}

namespace pychron::spectrometer {

struct SpectrometerBringup {
  // Register a BeamModel following the config's field table as "default"
  // before assembling. Leave false for real hardware or a caller-built beam.
  bool sim_beam_from_table = false;
  // Refuse a config that is not simulated (see is_simulated) before anything
  // is assembled, any transport opened or the beam registry touched.
  bool require_sim = false;
  // With sim_beam_from_table: the simulated line whose source volume that
  // beam reads (feed_beam_from_line, once the spectrometer is assembled).
  // Null: no line, and the beam's fixed gas. On the same clock as the
  // spectrometer. The SimSystem itself need not outlive the beam (the beam
  // holds it weakly), but the beam also refers to its clock, which in the
  // applications is the line's: they destroy the spectrometer and clear the
  // beam registry before the line for that reason.
  sim::SimSystem* line_sim = nullptr;
};

// True when nothing in the config can reach hardware: every declared
// transport has kind sim (or none is declared) and every driver's kind is a
// simulator kind, i.e. carries the "sim_" prefix all libs/sim drivers register
// under.
bool is_simulated(const cfg::SpectrometerData& data);

// BeamSettings whose table_value follows the config's active field table
// (mass / 8 where the table has no value) and nominal_hv from the config.
sim::BeamSettings beam_settings_from_config(const cfg::SpectrometerData& data);

// Joins a simulated beam to a simulated line. The beam's gas becomes what the
// line's spectrometer volume holds at the instant of each reading
// (`SimSystem::beam_gas`); a line with no such volume leaves the beam its
// fixed gas. Each `[detectors.<name>]` of the line's settings is that
// detector's baseline and drift, and is applied whether or not the line has
// a spectrometer volume (a line loaded without its canvas has none, and its
// detectors may still be given baselines). The line's seed, when it was
// given one (`[defaults] seed`, or by whoever built the line: any seed but
// `SimSettings`' default), becomes the beam's, so that one seed serves the
// gauges and the detectors; with none given the beam keeps the seed it was
// built with. Call this once the beam has its detectors (the sim drivers add
// them as the spectrometer is assembled).
// Config errors: a detector name the beam does not have (naming the file,
// the key and the detectors there are), a baseline that is not finite, a
// beam and a line on different clocks (a reading's instant is the line's
// time). Everything is checked before anything is changed: on an error the
// beam is as it was. The line's SimSystem need not outlive the beam, which
// then reads its baselines and no gas; the beam's clock must. Lock order:
// the beam's mutex, then the line's; the line never calls a beam.
Result<void> feed_beam_from_line(sim::BeamModel& beam, sim::SimSystem& line);

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
