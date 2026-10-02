#include "pychron/experiment/conditionals/queue_actions.hpp"

namespace pychron::experiment {

namespace {

bool same_aliquot(const RunSpec& a, const RunSpec& b) {
  return a.id.identifier == b.id.identifier && a.id.aliquot == b.id.aliquot;
}

// Following unknowns of the current run's aliquot, in order, stopping at the
// first run of another identifier (pychron's "consecutive" rule).
std::vector<std::size_t> aliquot_rows(const ExperimentQueue& q, std::size_t current) {
  std::vector<std::size_t> out;
  const auto& runs = q.runs();
  for (std::size_t i = current + 1; i < runs.size(); ++i) {
    if (runs[i].id.identifier != runs[current].id.identifier) break;
    if (runs[i].skip || runs[i].id.type != AnalysisType::Unknown || !same_aliquot(runs[i], runs[current])) continue;
    out.push_back(i);
  }
  return out;
}

Result<void> set_skip(ExperimentQueue& q, std::size_t row, QueueChange& change) {
  RunSpec r = q.runs()[row];
  r.skip = true;
  if (auto res = q.replace(row, std::move(r)); !res) return res;
  change.skipped.push_back(row);
  return {};
}

}  // namespace

AnalysisType blank_type_for(AnalysisType type) noexcept {
  switch (type) {
    case AnalysisType::Air:
    case AnalysisType::BlankAir: return AnalysisType::BlankAir;
    case AnalysisType::Cocktail:
    case AnalysisType::BlankCocktail: return AnalysisType::BlankCocktail;
    case AnalysisType::BlankExtractionLine: return AnalysisType::BlankExtractionLine;
    default: return AnalysisType::BlankUnknown;
  }
}

BlankFactory default_blank_factory(IdentifierRules rules) {
  return [rules = std::move(rules)](const RunSpec& current, AnalysisType blank_type) {
    RunSpec b;
    b.id.type = blank_type;
    b.id.identifier = rules.prefix_for(blank_type);
    b.extraction = current.extraction;
    b.measurement = current.measurement;
    b.post_equilibration = current.post_equilibration;
    b.post_measurement = current.post_measurement;
    b.comment = "run_blank after " + current.id.identifier;
    return b;
  };
}

Result<QueueChange> apply_queue_action(ExperimentQueue& q, std::size_t current, const ActionSpec& a,
                                       const BlankFactory& blank) {
  using T = ActionSpec::Type;
  if (current >= q.size()) return fail(ErrorKind::Config, "current row " + std::to_string(current) + " out of range");
  if (!is_queue_action(a.type)) return fail(ErrorKind::Config, "'" + to_string(a) + "' is not a queue action");
  QueueChange change;
  change.description = to_string(a);
  const RunSpec cur = q.runs()[current];

  switch (a.type) {
    case T::SkipNext:
    case T::SkipN: {
      int n = a.type == T::SkipNext ? 1 : a.count;
      for (std::size_t i = current + 1; i < q.size() && n > 0; ++i) {
        if (q.runs()[i].skip) continue;
        if (auto r = set_skip(q, i, change); !r) return fail(r.error());
        --n;
      }
      break;
    }
    case T::SkipAliquot:
    case T::SkipToLastInAliquot: {
      auto rows = aliquot_rows(q, current);
      if (a.type == T::SkipToLastInAliquot && !rows.empty()) rows.pop_back();
      for (auto i : rows)
        if (auto r = set_skip(q, i, change); !r) return fail(r.error());
      break;
    }
    case T::SetExtract: {
      auto rows = aliquot_rows(q, current);
      for (std::size_t k = 0; k < rows.size() && k < a.steps.size(); ++k) {
        RunSpec r = q.runs()[rows[k]];
        if (a.percent) r.extraction.value *= 1.0 + a.steps[k] / 100.0;
        else r.extraction.value += a.steps[k];
        if (auto res = q.replace(rows[k], std::move(r)); !res) return fail(res.error());
        change.modified.push_back(rows[k]);
      }
      break;
    }
    case T::Repeat: {
      RunSpec copy = cur;
      copy.id.aliquot.reset();
      copy.skip = false;
      copy.end_after = false;
      if (auto r = q.insert(current + 1, std::move(copy)); !r) return fail(r.error());
      change.inserted = current + 1;
      break;
    }
    case T::RunBlank: {
      if (!blank) return fail(ErrorKind::Config, "run_blank needs a blank factory");
      if (auto r = q.insert(current + 1, blank(cur, blank_type_for(cur.id.type))); !r) return fail(r.error());
      change.inserted = current + 1;
      break;
    }
    default: break;
  }
  return change;
}

}  // namespace pychron::experiment
