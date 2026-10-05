#pragma once

// AnalysisRecord v1 (spec 8.3). Plain data; built by RecordBuilder, serialized
// deterministically by serialize.hpp. Derived values (age, K/Ca, yield) are
// never stored: they are reductions over this record.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pychron/reduction/fits.hpp"

namespace pychron::experiment::record {

// Bump when the serialized layout changes; parse rejects other versions.
// v2: typed conditionals provenance (conditionals spec 6.3), whiff result string.
inline constexpr int kRecordSchemaVersion = 2;

struct Identity {
  std::string uuid, identifier;
  int aliquot = 0;
  std::string step, analysis_type, timestamp;  // timestamp: ISO-8601 UTC
  int run_index = 0;
  std::string queue_uuid;
  friend bool operator==(const Identity&, const Identity&) = default;
};

struct SampleMeta {
  std::string sample, project, material, irradiation, level, position, pi, note;
  friend bool operator==(const SampleMeta&, const SampleMeta&) = default;
};

struct InstrumentMeta {
  std::string mass_spectrometer, extract_device, laboratory, analyst;
  std::string software_version, software_git_sha;
  friend bool operator==(const InstrumentMeta&, const InstrumentMeta&) = default;
};

// Float32 series; sigma is either empty or the same length as t.
struct Trace {
  std::vector<float> t, v, sigma;
  friend bool operator==(const Trace&, const Trace&) = default;
};

struct ExtractionSpecRec {
  double value = 0, duration = 0, cleanup = 0;
  std::string units, pattern;
  std::vector<int> positions;
  std::optional<double> cryo_temperature;  // the run's requested cryo_temp, kelvin
  friend bool operator==(const ExtractionSpecRec&, const ExtractionSpecRec&) = default;
};

struct ExtractionActuals {
  double value = 0, duration = 0, cleanup = 0, beam_diameter = 0;
  std::vector<int> positions;
  std::string pattern;
  std::map<std::string, double> pid_params;
  std::map<std::string, Trace> series;  // "response", "output", "setpoint", "cryo"
  std::vector<std::string> snapshot_refs;
  std::vector<std::vector<std::pair<double, double>>> grain_polygons;
  int pipette_counts = 0;
  std::optional<double> manometer_pressure;
  // The line cryostat's inputs (kelvin) read as the extraction ended; empty
  // without a cryostat or when the read failed (the run's log says why).
  std::map<std::string, double> cryo_measured;
  friend bool operator==(const ExtractionActuals&, const ExtractionActuals&) = default;
};

struct Extraction {
  ExtractionSpecRec spec;
  ExtractionActuals actuals;
  friend bool operator==(const Extraction&, const Extraction&) = default;
};

struct ScriptRef {
  std::string name, sha, text_ref;
  friend bool operator==(const ScriptRef&, const ScriptRef&) = default;
};

struct PlanRef {
  std::string template_name, version, effective_plan_toml;
  std::map<std::string, std::string> overrides;
  friend bool operator==(const PlanRef&, const PlanRef&) = default;
};

struct HookRef {
  std::string name, sha;
  friend bool operator==(const HookRef&, const HookRef&) = default;
};

struct Measurement {
  PlanRef plan;
  std::optional<HookRef> hook;
  std::map<std::string, ScriptRef> scripts;  // "extraction", "post_eq", "post_meas"
  friend bool operator==(const Measurement&, const Measurement&) = default;
};

struct SpectrometerRec {
  std::string state_hash, field_table_version;
  double integration_time = 0;
  std::map<std::string, double> deflections, gains, source_params;
  friend bool operator==(const SpectrometerRec&, const SpectrometerRec&) = default;
};

struct DataSeries {
  std::string iso, det, kind;  // kind: "signal", "baseline", ...
  Trace trace;
  friend bool operator==(const DataSeries&, const DataSeries&) = default;
};

struct Data {
  std::vector<DataSeries> series;
  double time_zero = 0;
  std::map<std::string, int> counts;
  friend bool operator==(const Data&, const Data&) = default;
};

inline bool same_fit(const reduction::FitSpec& a, const reduction::FitSpec& b) {
  return a.kind == b.kind && a.error == b.error && a.degree == b.degree && a.outliers.enabled == b.outliers.enabled &&
         a.outliers.iterations == b.outliers.iterations && a.outliers.std_devs == b.outliers.std_devs;
}

struct InterceptResult {
  reduction::Intercept intercept;
  reduction::FitSpec fit;
  friend bool operator==(const InterceptResult& a, const InterceptResult& b) {
    return a.intercept.value == b.intercept.value && a.intercept.error == b.intercept.error &&
           a.intercept.n_used == b.intercept.n_used && a.intercept.filtered_idx == b.intercept.filtered_idx &&
           a.intercept.residual_sd == b.intercept.residual_sd && same_fit(a.fit, b.fit);
  }
};

struct BaselineResult {
  double value = 0, error = 0;
  reduction::FitSpec fit;
  friend bool operator==(const BaselineResult& a, const BaselineResult& b) {
    return a.value == b.value && a.error == b.error && same_fit(a.fit, b.fit);
  }
};

struct Results {
  std::map<std::string, InterceptResult> intercepts;  // by isotope
  std::map<std::string, BaselineResult> baselines;    // by detector
  std::string blanks_ref;
  std::map<std::string, double> icfactors;  // by detector; 1.0 when absent
  std::string whiff;  // run_remainder | pump | abort; empty without a whiff
  friend bool operator==(const Results&, const Results&) = default;
};

struct InstalledConditional {
  std::string id, name, kind, level, location, check;  // check: canonical, after window/mapper
  int start = 0, frequency = 1, ntrips = 1, window = 0;  // window 0 = none
  std::string mapper;
  std::vector<std::string> analysis_types;
  double abbreviated_count_ratio = 1.0;
  std::string action;
  bool resume = false, truncate = false, terminate = false;
  friend bool operator==(const InstalledConditional&, const InstalledConditional&) = default;
};

struct TrippedConditional {
  std::string id, name, kind, check, action;
  int reading = 0, count = 0;
  double t = 0, value = 0;
  std::map<std::string, double> context;  // every metric the check read
  friend bool operator==(const TrippedConditional&, const TrippedConditional&) = default;
};

struct ConditionalErrorRec {
  std::string name, message;
  int count = 0;
  friend bool operator==(const ConditionalErrorRec&, const ConditionalErrorRec&) = default;
};

struct Conditionals {
  std::vector<InstalledConditional> installed;
  std::vector<TrippedConditional> tripped;
  std::vector<ConditionalErrorRec> errors;
  friend bool operator==(const Conditionals&, const Conditionals&) = default;
};

struct Event {
  double t = 0;
  std::string kind;  // "state", "alarm", "peak_center", "environment", "note" (what the run said)
  std::string detail;
  friend bool operator==(const Event&, const Event&) = default;
};

struct Provenance {
  int schema_version = kRecordSchemaVersion;
  std::string sha;  // sha256 hex of the canonical record with this field empty
  std::vector<std::string> persister_refs;
  friend bool operator==(const Provenance&, const Provenance&) = default;
};

struct AnalysisRecord {
  Identity identity;
  SampleMeta sample;
  InstrumentMeta instrument;
  Extraction extraction;
  Measurement measurement;
  SpectrometerRec spectrometer;
  Data data;
  Results results;
  Conditionals conditionals;
  std::vector<Event> events;
  Provenance provenance;
  friend bool operator==(const AnalysisRecord&, const AnalysisRecord&) = default;
};

// icfactor for `det`, 1.0 when none recorded.
double icfactor(const Results& r, const std::string& det);

}  // namespace pychron::experiment::record
