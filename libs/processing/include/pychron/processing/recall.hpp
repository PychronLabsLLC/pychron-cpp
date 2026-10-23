#pragma once

// The recall model (design section 11.3): the tables and plots of the recall
// window, computed Qt-free from a reduced analysis and its raw data.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/processing/reduced.hpp"
#include "pychron/processing/scene.hpp"

namespace pychron::processing {

struct RecallRow {
  std::string name;
  std::optional<Value> value;  // absent: show `text`
  std::string units;
  std::string text;            // non-numeric values
  std::string note;            // tooltip
};

struct RecallSection {
  std::string title;
  std::vector<RecallRow> rows;
};

struct RecallIsotope {
  std::string key, isotope, detector, fit, baseline_fit, blank_source;
  int n = 0;
  std::map<Stage, Value> stages;
};

// One line of a domain's error budget (Domain::error_budget), largest first.
struct RecallErrorComponent {
  std::string name;  // "Ar40", "Ar40 bs", "J", "lambda_k", ...
  double percent = 0.0;
};

struct RecallModel {
  std::string title;  // "66001-01A  FC-2  unknown"
  std::vector<std::string> header;  // date, spectrometer, extraction device, tag
  RecallSection identity;      // identifier, sample, project, irradiation, ...
  RecallSection extraction;    // value, duration, cleanup, positions, ...
  RecallSection spectrometer;  // gains, deflections, source, peak centers
  std::vector<RecallIsotope> isotopes;  // core stages
  std::string reduction_note;  // why the domain's values are missing (ReducedAnalysis::reduction_error)
};

// The neutral tables; a domain's sections and budget are Domain::recall_sections
// and Domain::error_budget.
RecallModel make_recall_model(const ReducedAnalysis& analysis);

// Isotope evolutions as a scene: one panel per series of `kind` (isotopes for
// signals and sniffs, detectors for baselines), the measured points, and for
// signals the stored fit refitted on the raw data with its envelope and the
// intercept at t = 0.
Scene make_evolution_scene(const Analysis& analysis, const RawData& raw, SeriesKind kind,
                           const std::vector<std::string>& keys = {});

}  // namespace pychron::processing
