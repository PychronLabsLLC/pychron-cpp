#pragma once

// Converts a legacy Pychron extraction line (setupfiles extractionline/,
// canvas2D/, devices/) into extraction_line.toml and canvas.toml (legacy
// extraction-line survey, docs/superpowers/specs/2026-10-03-legacy-
// extraction-line-survey.md).
//
// Valves, manual valves, pipettes and interlocks map one to one. Each legacy
// actuator becomes a driver: NGXGPActuator -> ngx_valves on its host and
// port; any other class -> a sim_valves stand-in on a simulated transport,
// with the legacy class and endpoint in a comment, so the line loads and
// runs in simulation. The canvas is rescaled from world units (y up) to
// pixels. What could not be carried over is listed in `notes`.
//
// Both files are loaded with the real loaders, and the canvas checked
// against the line, before they are returned.

#include <filesystem>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::setup {

struct LegacyLine {
  std::string line_toml;    // extraction_line.toml
  std::string canvas_toml;  // canvas.toml
  std::vector<std::string> read;   // the legacy files used
  std::vector<std::string> notes;  // what was not carried over, stand-ins, dropped elements
};

// `folder` is a setupfiles folder (holding extractionline/ and canvas2D/),
// or one of those two. Config error when no valve file is found or the
// converted files do not load.
Result<LegacyLine> import_legacy_line(const std::filesystem::path& folder);

}  // namespace pychron::setup
