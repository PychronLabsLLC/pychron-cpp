#include "pychron/experiment/model/rules.hpp"

namespace pychron::experiment {

FieldRules rules_for(AnalysisType type) {
  switch (type) {
    case AnalysisType::Pause:
      return {.extraction = false, .heating = false, .position = false, .measurement = false};
    case AnalysisType::Air:
    case AnalysisType::Cocktail:
    case AnalysisType::BlankAir:
    case AnalysisType::BlankCocktail:
    case AnalysisType::Background:
    case AnalysisType::DetectorIC:
      return {.extraction = true, .heating = false, .position = false, .measurement = true};
    case AnalysisType::Unknown:
    case AnalysisType::BlankUnknown:
    case AnalysisType::BlankExtractionLine:
    case AnalysisType::Degas:
      break;
  }
  return {};
}

namespace {

void add(std::vector<Issue>& out, int run, std::string field, std::string msg) {
  out.push_back({run, std::move(field), std::move(msg)});
}

}  // namespace

std::vector<Issue> validate_run(const RunSpec& run, const IdentifierRules& ids, int index) {
  std::vector<Issue> out;
  const auto& id = run.id;
  const auto& e = run.extraction;

  if (auto r = ids.validate_identifier(id.identifier); !r) add(out, index, "identifier", r.error().what);
  if (ids.classify(id.identifier) != id.type)
    add(out, index, "type", "type '" + std::string(to_string(id.type)) + "' does not match identifier '" + id.identifier + "'");
  if (id.aliquot && *id.aliquot < 0) add(out, index, "aliquot", "aliquot must be >= 0");
  if (!ids.valid_step(id.step)) add(out, index, "step", "step '" + id.step + "' is malformed");
  if (ids.is_special(id.identifier) && (id.aliquot || !id.step.empty()))
    add(out, index, "aliquot", "special identifiers get aliquot and step assigned at run start");

  const FieldRules f = rules_for(id.type);
  const std::string t(to_string(id.type));
  auto forbid = [&](bool present, const char* field) {
    if (present) add(out, index, std::string("extraction.") + field, std::string(field) + " not allowed for " + t + " runs");
  };
  if (!f.extraction) {
    forbid(!e.device.empty(), "device");
    forbid(!e.script.empty(), "script");
    forbid(e.duration.count() != 0 || e.cleanup.count() != 0 || e.pre_cleanup.count() != 0 || e.post_cleanup.count() != 0,
           "duration");
  }
  if (!f.heating) {
    forbid(e.value != 0, "value");
    forbid(e.pattern.has_value(), "pattern");
    forbid(e.beam_diameter.has_value(), "beam_diameter");
    forbid(e.ramp_rate.has_value(), "ramp_rate");
    forbid(e.ramp.count() != 0, "ramp");
    forbid(e.cryo_temp.has_value(), "cryo_temp");
  }
  if (!f.position) forbid(e.position.has_value(), "position");
  if (f.measurement && run.measurement.plan.empty() && !run.measurement.hook)
    add(out, index, "measurement.plan", "measurement plan required for " + t + " runs");
  if (!f.measurement && !run.measurement.plan.empty())
    add(out, index, "measurement.plan", "measurement plan not allowed for " + t + " runs");

  for (auto [name, d] : {std::pair{"duration", e.duration}, {"cleanup", e.cleanup}, {"pre_cleanup", e.pre_cleanup},
                         {"post_cleanup", e.post_cleanup}, {"ramp", e.ramp}, {"delay_after", run.delay_after}})
    if (d.count() < 0) add(out, index, name, std::string(name) + " must be >= 0");
  if (e.position)
    for (int h : e.position->holes)
      if (h < 0) add(out, index, "extraction.position", "negative hole");
  if (run.weight && *run.weight < 0) add(out, index, "weight", "weight must be >= 0");
  return out;
}

std::vector<Issue> validate_queue(const QueueSpec& queue, const IdentifierRules& ids) {
  std::vector<Issue> out;
  if (queue.mass_spectrometer.empty()) add(out, -1, "queue.mass_spectrometer", "mass_spectrometer is required");
  for (std::size_t i = 0; i < queue.runs.size(); ++i) {
    auto issues = validate_run(queue.runs[i], ids, static_cast<int>(i));
    out.insert(out.end(), issues.begin(), issues.end());
  }
  return out;
}

}  // namespace pychron::experiment
