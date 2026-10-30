#pragma once

// The simulated lab of a line, built from its config and canvas without the
// line (sim serve spec, docs/superpowers/specs/2026-10-09-sim-serve-design.md
// section 4.2): what ExtractionLine runs its `kind = "sim"` transports on, and
// what sim serve puts on sockets. One path builds it for both.
//
// The canvas's volumes and valves, each stage as the gas sees it (a pump
// pumps, a tank holds air, ...), the simulator's numbers from
// LineSimOptions::sim with a sim.toml over them. Two valves drawn joined
// directly have a small pipe volume between them, `<a>~<b>`.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/canvas/canvas.hpp"

namespace pychron::systems {

// Something the builder has to say: logged by the line, printed by sim serve.
struct LineSimNote {
  LogLevel level = LogLevel::Info;
  std::string message;
};

struct LineSimOptions {
  sim::SimSettings sim;  // the simulator's numbers before any sim.toml
  // A sim.toml read over `sim`. Unset: with a canvas, the config's
  // `[sim] file` (relative to the line's file); set and empty: none.
  std::optional<std::filesystem::path> sim_file;
  // A sim.toml beside the line that was left alone because no canvas was
  // loaded (sim_file_beside), for the note that says so.
  std::filesystem::path sim_beside_skipped;
};

struct LineSim {
  std::unique_ptr<sim::SimSystem> system;
  // A sim.toml not read (info), a second spectrometer (warn), the valves
  // that carry no gas (info).
  std::vector<LineSimNote> notes;
};

// Config error on a canvas valve with `~` in its name, on what the sim.toml
// refuses, on a cryotrap whose cryostat is not a lakeshore driver of the line,
// and on the simulator's own refusal of the network (SimSystem::build_error).
// Making hooks may still refuse a gauge's volume: whoever makes them checks
// build_error() again.
Result<LineSim> build_line_sim(const config::SystemConfig& config, const canvas::Canvas* canvas, const Clock& clock,
                               LineSimOptions options = {});

// The sim.toml beside `system_file`, when there is one: to read, with a
// canvas (its names are the canvas's); else to say it was left alone.
struct SimFileBeside {
  std::optional<std::filesystem::path> read;
  std::filesystem::path skipped;
};
SimFileBeside sim_file_beside(const std::filesystem::path& system_file, bool has_canvas);

}  // namespace pychron::systems
