#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/error.hpp"
#include "pychron/systems/canvas/canvas.hpp"

namespace pychron::canvas {

// Full outcome of a load: `canvas` is set only when `diagnostics` is empty.
struct CanvasLoadReport {
  std::optional<Canvas> canvas;
  std::vector<config::Diagnostic> diagnostics;

  bool ok() const noexcept { return canvas.has_value(); }
};

// Parse + schema-check + internal validation, collecting every problem:
//  - required fields, types, enum values, no unknown fields or sections
//  - element names unique across every element kind
//  - connection/elbow/tee/cross endpoints name a plumbing element (valve,
//    manual/rough valve, gauge, stage, pipette); switches are not plumbing
//  - no connection from an element to itself
// Loading is all-or-nothing. Cross-checks against `extraction_line.toml` live
// in cross_validate.hpp.
CanvasLoadReport load_canvas_report_from_string(std::string_view toml, std::string_view name);
CanvasLoadReport load_canvas_report(const std::filesystem::path& path);

// Result form; the Error (kind Config) lists every diagnostic, one per line.
Result<Canvas> load_canvas_from_string(std::string_view toml, std::string_view name);
Result<Canvas> load_canvas(const std::filesystem::path& path);

}  // namespace pychron::canvas
