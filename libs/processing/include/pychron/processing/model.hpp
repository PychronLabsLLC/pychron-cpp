#pragma once

// The analysis model for browsing, recall and figures (data browsing and
// visualization design, section 7.3). Plain immutable values: a source builds
// an Analysis once and every consumer shares it as shared_ptr<const Analysis>.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/fits.hpp"

namespace pychron::processing {

// A value with a 1-sigma error.
struct Value {
  double value = 0.0;
  double error = 0.0;
  friend bool operator==(const Value&, const Value&) = default;
};

// The stored state of one isotope (dvc intercepts/baselines/blanks/icfactors
// rows, or an AnalysisRecord's results).
struct IsotopeData {
  std::string key;       // unique per analysis: "Ar40", or "H1:Ar40" for multi-detector runs
  std::string isotope;   // "Ar40"
  std::string detector;  // "H1"
  Value intercept;
  Value baseline;
  Value blank;
  Value ic_factor{1.0, 0.0};
  std::optional<reduction::FitSpec> fit;
  std::optional<reduction::FitSpec> baseline_fit;
  int n = 0;  // points in the intercept fit
  bool include_baseline_error = false;
  std::string blank_source;  // fit name or reference run id
};

struct ExtractionInfo {
  std::optional<double> value, duration, cleanup, weight, beam_diameter;
  std::string units, pattern;
  std::vector<int> positions;
};

struct PeakCenterInfo {
  std::string detector;
  double center = 0.0;
  std::optional<double> resolution;
};

// What reduce() needs beyond the stored values. Any part may be absent: a
// record directory knows no flux, so its unknowns have no age.
struct ReductionContext {
  std::optional<reduction::Flux> flux;
  std::optional<reduction::ProductionRatios> production;
  std::vector<reduction::Dose> chronology;
  std::optional<reduction::ReductionConstants> constants;  // absent: the run's settings decide
  std::optional<reduction::Measured> fixed_k3739;
};

struct Analysis {
  std::string uuid;
  std::string identifier;
  int aliquot = 0;
  int increment = -1;  // -1: no step
  std::string runid;   // identifier-aliquot[step]
  std::string analysis_type;
  double timestamp = 0.0;  // UTC epoch seconds
  std::string mass_spectrometer, extract_device;
  std::string sample, project, material, principal_investigator;
  std::string irradiation, level, position, load;
  std::string repository, analyst, comment;
  std::string tag = "ok";

  ExtractionInfo extraction;
  std::vector<IsotopeData> isotopes;
  std::map<std::string, double> gains, deflections, source;
  std::map<std::string, double> environmentals;  // lab_temperature, lab_humidity, ...
  std::vector<PeakCenterInfo> peak_centers;
  ReductionContext context;

  const IsotopeData* find_isotope(std::string_view key) const;
  // The first isotope named `isotope` (exact key preferred).
  const IsotopeData* find_by_isotope(std::string_view isotope) const;
  // Step letters for `increment` ("" for -1, "A" for 0, ..., "AA" for 26).
  std::string step() const;
};

using AnalysisPtr = std::shared_ptr<const Analysis>;

// Raw series of one analysis, loaded on demand (IAnalysisSource::load_raw).
enum class SeriesKind { Signal, Baseline, Sniff };
std::string_view to_string(SeriesKind kind) noexcept;

struct RawSeries {
  SeriesKind kind = SeriesKind::Signal;
  std::string key;       // isotope key; the detector for baselines
  std::string detector;
  std::vector<double> t, v;
};

struct RawData {
  std::vector<RawSeries> series;
  const RawSeries* find(SeriesKind kind, std::string_view key) const;
};

// identifier-aliquot(2 digits)[step letters], legacy make_runid.
std::string make_runid(const std::string& identifier, int aliquot, int increment);
std::string step_letters(int increment);

}  // namespace pychron::processing
