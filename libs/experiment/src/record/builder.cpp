#include "pychron/experiment/record/builder.hpp"

#include <set>

#include "pychron/experiment/record/serialize.hpp"

namespace pychron::experiment::record {

std::string_view to_string(Phase p) noexcept {
  switch (p) {
    case Phase::Identity: return "identity";
    case Phase::Extraction: return "extraction";
    case Phase::Measurement: return "measurement";
    case Phase::Data: return "data";
    case Phase::Results: return "results";
  }
  return "unknown";
}

RecordBuilder& RecordBuilder::set_identity(Identity v) { rec_.identity = std::move(v); return *this; }
RecordBuilder& RecordBuilder::set_sample(SampleMeta v) { rec_.sample = std::move(v); return *this; }
RecordBuilder& RecordBuilder::set_instrument(InstrumentMeta v) { rec_.instrument = std::move(v); return *this; }
RecordBuilder& RecordBuilder::set_extraction(Extraction v) { rec_.extraction = std::move(v); return *this; }
RecordBuilder& RecordBuilder::set_measurement(Measurement v) { rec_.measurement = std::move(v); return *this; }
RecordBuilder& RecordBuilder::set_spectrometer(SpectrometerRec v) { rec_.spectrometer = std::move(v); return *this; }
RecordBuilder& RecordBuilder::add_series(DataSeries v) { rec_.data.series.push_back(std::move(v)); return *this; }
RecordBuilder& RecordBuilder::set_time_zero(double t) { rec_.data.time_zero = t; return *this; }
RecordBuilder& RecordBuilder::set_count(const std::string& key, int n) { rec_.data.counts[key] = n; return *this; }
RecordBuilder& RecordBuilder::set_intercept(const std::string& iso, InterceptResult v) {
  rec_.results.intercepts[iso] = std::move(v);
  return *this;
}
RecordBuilder& RecordBuilder::set_baseline(const std::string& det, BaselineResult v) {
  rec_.results.baselines[det] = std::move(v);
  return *this;
}
RecordBuilder& RecordBuilder::set_blanks_ref(std::string ref) { rec_.results.blanks_ref = std::move(ref); return *this; }
RecordBuilder& RecordBuilder::set_icfactor(const std::string& det, double v) { rec_.results.icfactors[det] = v; return *this; }
RecordBuilder& RecordBuilder::set_whiff(double v) { rec_.results.whiff = v; return *this; }
RecordBuilder& RecordBuilder::set_conditionals(Conditionals v) { rec_.conditionals = std::move(v); return *this; }
RecordBuilder& RecordBuilder::add_event(Event v) { rec_.events.push_back(std::move(v)); return *this; }
RecordBuilder& RecordBuilder::add_persister_ref(std::string ref) {
  rec_.provenance.persister_refs.push_back(std::move(ref));
  return *this;
}

namespace {

bool trace_ok(const Trace& t) {
  return !t.t.empty() && t.t.size() == t.v.size() && (t.sigma.empty() || t.sigma.size() == t.t.size());
}

}  // namespace

std::vector<std::string> RecordBuilder::problems(Phase phase) const {
  std::vector<std::string> out;
  const auto need = [&](bool ok, std::string what) {
    if (!ok) out.push_back(std::string(to_string(phase)) + ": " + std::move(what));
  };
  const auto& r = rec_;
  switch (phase) {
    case Phase::Identity:
      need(!r.identity.uuid.empty(), "identity.uuid missing");
      need(!r.identity.identifier.empty(), "identity.identifier missing");
      need(!r.identity.analysis_type.empty(), "identity.analysis_type missing");
      need(!r.identity.timestamp.empty(), "identity.timestamp missing");
      need(!r.instrument.mass_spectrometer.empty(), "instrument.mass_spectrometer missing");
      break;
    case Phase::Extraction:
      need(!r.extraction.spec.units.empty(), "extraction.spec.units missing");
      need(r.extraction.actuals.duration >= 0, "extraction.actuals.duration negative");
      for (const auto& [k, tr] : r.extraction.actuals.series)
        need(trace_ok(tr), "extraction.actuals.series." + k + " empty or length mismatch");
      break;
    case Phase::Measurement:
      need(!r.measurement.plan.template_name.empty(), "measurement.plan.template missing");
      need(!r.measurement.plan.effective_plan_toml.empty(), "measurement.plan.effective_plan_toml missing");
      need(r.measurement.scripts.count("extraction") == 1, "measurement.scripts.extraction missing");
      need(!r.spectrometer.state_hash.empty(), "spectrometer.state_hash missing");
      need(r.spectrometer.integration_time > 0, "spectrometer.integration_time not positive");
      need(!r.spectrometer.field_table_version.empty(), "spectrometer.field_table_version missing");
      break;
    case Phase::Data:
      need(!r.data.series.empty(), "data.series empty");
      for (const auto& s : r.data.series)
        need(trace_ok(s.trace), "data.series " + s.iso + "/" + s.det + "/" + s.kind + " empty or length mismatch");
      break;
    case Phase::Results: {
      std::set<std::string> signal_isos, baseline_dets;
      for (const auto& s : r.data.series) {
        if (s.kind == "signal") signal_isos.insert(s.iso);
        if (s.kind == "baseline") baseline_dets.insert(s.det);
      }
      for (const auto& iso : signal_isos) need(r.results.intercepts.count(iso) == 1, "results.intercepts." + iso + " missing");
      for (const auto& det : baseline_dets) need(r.results.baselines.count(det) == 1, "results.baselines." + det + " missing");
      break;
    }
  }
  return out;
}

Result<void> RecordBuilder::check(Phase phase) const {
  const auto p = problems(phase);
  if (p.empty()) return {};
  std::string msg;
  for (const auto& s : p) msg += (msg.empty() ? "" : "; ") + s;
  return fail(ErrorKind::Config, std::move(msg));
}

Result<AnalysisRecord> RecordBuilder::finalize() const {
  std::string msg;
  for (auto phase : kAllPhases)
    for (const auto& s : problems(phase)) msg += (msg.empty() ? "" : "; ") + s;
  if (!msg.empty()) return fail(ErrorKind::Config, "incomplete record: " + msg);
  AnalysisRecord out = rec_;
  out.provenance.schema_version = kRecordSchemaVersion;
  out.provenance.sha = compute_sha(out);
  return out;
}

}  // namespace pychron::experiment::record
