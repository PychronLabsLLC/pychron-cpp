#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pychron/experiment/model/identifiers.hpp"
#include "pychron/experiment/model/types.hpp"

namespace pychron::experiment {

struct RunIdentity {
  std::string identifier;
  std::optional<int> aliquot;  // user-fixed; otherwise assigned at run start
  std::string step;
  AnalysisType type = AnalysisType::Unknown;
  friend bool operator==(const RunIdentity&, const RunIdentity&) = default;
};

struct ExtractionSpec {
  std::string device;
  std::optional<Position> position;
  double value = 0;
  Unit units = Unit::Watts;
  Duration duration{0}, cleanup{0}, pre_cleanup{0}, post_cleanup{0};
  std::optional<Pattern> pattern;
  std::optional<double> beam_diameter, ramp_rate;
  Duration ramp{0};
  std::optional<double> cryo_temp;
  std::string script;
  ScriptOptions options;
  friend bool operator==(const ExtractionSpec&, const ExtractionSpec&) = default;
};

struct MeasurementRef {
  std::string plan;
  ParamOverrides overrides;
  std::optional<std::string> hook;
  friend bool operator==(const MeasurementRef&, const MeasurementRef&) = default;
};

struct RunSpec {
  RunIdentity id;
  ExtractionSpec extraction;
  MeasurementRef measurement;
  std::optional<std::string> post_equilibration, post_measurement;
  Overlap overlap;
  Duration delay_after{0};
  std::vector<ConditionalRef> conditionals;
  std::string comment;
  std::optional<double> weight;
  bool skip = false, end_after = false;
  SampleInfo sample;
  friend bool operator==(const RunSpec&, const RunSpec&) = default;
};

struct QueueSpec {
  std::string name, mass_spectrometer, extract_device, tray, load, username, email;
  Delays delays;
  std::string queue_conditionals;
  std::string repository;
  std::vector<RunSpec> runs;
};

inline constexpr int kQueueSchemaVersion = 1;

// Extraction time only (pre_cleanup + ramp + duration + cleanup + post_cleanup + delay_after).
// Measurement time is supplied by the caller (it depends on the plan).
Duration estimate_run(const RunSpec& run, Duration measurement);

}  // namespace pychron::experiment
