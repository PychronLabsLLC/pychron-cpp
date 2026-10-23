#pragma once

// The pipeline of one tab of the experiment window's References dock
// (docs/superpowers/specs/2026-10-13-experiment-reference-series-design.md):
// the newest blanks or airs of a spectrometer, drawn as a time series. A pure
// function of the query and the figure's options; the dock submits it to the
// data workspace's ProcessingBridge and draws what comes back.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/processing/options.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::ui {

enum class ReferenceKind { Blanks, Airs };

struct ReferenceQuery {
  ReferenceKind kind = ReferenceKind::Blanks;
  std::string mass_spectrometer;  // empty: any
  int last_n = 50;                // the dock offers 5 to 500
  std::optional<double> since;    // UTC epoch s; the reset point (an analysis at it is in)
};

// The id of the figure node of reference_pipeline(): the target a dock submits.
inline constexpr const char* kReferenceFigure = "figure";

// The analysis types a tab draws: the five blank kinds, or air.
[[nodiscard]] std::vector<std::string> analysis_types(ReferenceKind kind);
// The time-series preset a tab starts on: "Blanks" or "Air monitor".
[[nodiscard]] std::string_view starting_preset(ReferenceKind kind);
// select (the query) -> reduce -> group (key = analysis_type) -> time_series
// (with `figure` as its options), from `registry`: the one it runs on (the
// bridge's domain's units).
[[nodiscard]] processing::Pipeline reference_pipeline(const processing::UnitRegistry& registry,
                                                      const ReferenceQuery& query, const processing::Options& figure);

}  // namespace pychron::ui
