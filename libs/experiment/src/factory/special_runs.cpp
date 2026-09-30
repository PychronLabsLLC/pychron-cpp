#include "pychron/experiment/factory/special_runs.hpp"

namespace pychron::experiment {

std::vector<RunSpec> insert_frequency(const std::vector<RunSpec>& runs, const RunSpec& special, const Frequency& freq) {
  auto counts = [&](std::size_t i) {
    const RunSpec& r = runs[i];
    if (i < freq.first || i >= freq.last || r.skip) return false;
    return freq.count_all || r.id.type == AnalysisType::Unknown;
  };
  std::size_t first = runs.size(), last = runs.size();
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (!counts(i)) continue;
    if (first == runs.size()) first = i;
    last = i;
  }
  if (first == runs.size()) return runs;

  std::vector<RunSpec> out;
  out.reserve(runs.size() * 2);
  int n = 0;
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (i == first && freq.before) out.push_back(special);
    out.push_back(runs[i]);
    if (!counts(i)) continue;
    ++n;
    const bool periodic = freq.every > 0 && n % freq.every == 0;
    if (periodic || (i == last && freq.after)) out.push_back(special);
  }
  return out;
}

}  // namespace pychron::experiment
