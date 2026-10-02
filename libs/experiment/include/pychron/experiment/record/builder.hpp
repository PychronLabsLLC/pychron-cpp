#pragma once

// Incremental AnalysisRecord construction. Each run phase fills its section and
// can be checked on its own; finalize() (called from SavePhase) requires every
// phase to be complete, stamps provenance, and computes the record sha.

#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::record {

enum class Phase { Identity, Extraction, Measurement, Data, Results };

inline constexpr Phase kAllPhases[] = {Phase::Identity, Phase::Extraction, Phase::Measurement, Phase::Data,
                                       Phase::Results};

std::string_view to_string(Phase p) noexcept;

class RecordBuilder {
 public:
  RecordBuilder& set_identity(Identity v);
  RecordBuilder& set_sample(SampleMeta v);
  RecordBuilder& set_instrument(InstrumentMeta v);
  RecordBuilder& set_extraction(Extraction v);
  RecordBuilder& set_measurement(Measurement v);
  RecordBuilder& set_spectrometer(SpectrometerRec v);
  RecordBuilder& add_series(DataSeries v);
  RecordBuilder& set_time_zero(double t);
  RecordBuilder& set_count(const std::string& key, int n);
  RecordBuilder& set_intercept(const std::string& iso, InterceptResult v);
  RecordBuilder& set_baseline(const std::string& det, BaselineResult v);
  RecordBuilder& set_blanks_ref(std::string ref);
  RecordBuilder& set_icfactor(const std::string& det, double v);
  RecordBuilder& set_whiff(std::string result);  // run_remainder | pump | abort
  RecordBuilder& set_conditionals(Conditionals v);
  RecordBuilder& add_event(Event v);
  RecordBuilder& add_persister_ref(std::string ref);

  // Missing/invalid items for `phase`; empty when the phase is complete.
  std::vector<std::string> problems(Phase phase) const;
  Result<void> check(Phase phase) const;

  const AnalysisRecord& draft() const noexcept { return rec_; }

  // Fails (ErrorKind::Config) listing every problem in every phase.
  Result<AnalysisRecord> finalize() const;

 private:
  AnalysisRecord rec_;
};

}  // namespace pychron::experiment::record
