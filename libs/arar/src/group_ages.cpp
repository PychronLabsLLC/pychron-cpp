#include "pychron/arar/group_ages.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "pychron/arar/arar_reduction.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::processing {

namespace r = reduction;

namespace {

// The age of an item group_plateau or group_weighted_mean is given: one with ages (has_ages).
const r::UFloat& age_of(const DatasetItem& item) { return arar::reduced_of(*item.analysis)->arar.ages->age; }

}  // namespace

double k39_of(const DatasetItem& item) {
  const arar::ArArReduced* reduced = arar::reduced_of(*item.analysis);
  if (!reduced) return std::numeric_limits<double>::quiet_NaN();
  return reduced->arar.f.interference.k39.nominal();
}

bool has_ages(const DatasetItem& item) {
  const arar::ArArReduced* reduced = arar::reduced_of(*item.analysis);
  return reduced && reduced->arar.ages;
}

GroupItems step_ordered(GroupItems items) {
  std::stable_sort(items.begin(), items.end(), [](const DatasetItem* a, const DatasetItem* b) {
    const auto& x = *a->analysis->analysis;
    const auto& y = *b->analysis->analysis;
    if (x.increment != y.increment) return x.increment < y.increment;
    return x.timestamp < y.timestamp;
  });
  return items;
}

std::optional<GroupPlateau> group_plateau(const GroupItems& reduced, const r::PlateauCriteria& criteria,
                                          r::PlateauWeighting weighting, r::MeanErrorKind error) {
  std::vector<double> vs, es, ws;
  GroupPlateau out;
  for (const auto* it : reduced) {
    const double k = k39_of(*it);
    if (!(k > 0)) continue;
    const r::UFloat& age = age_of(*it);
    vs.push_back(age.nominal());
    es.push_back(age.std_dev());
    ws.push_back(k);
    out.steps.push_back(it);
  }
  const std::size_t n = out.steps.size();
  // NOLINTNEXTLINE(modernize-avoid-c-arrays): find_plateau takes a span of bool, which a vector<bool> cannot give
  std::unique_ptr<bool[]> ex(new bool[std::max<std::size_t>(1, n)]);
  for (std::size_t i = 0; i < n; ++i) ex[i] = out.steps[i]->exclusion.excluded();
  const std::span<const bool> excluded(ex.get(), n);
  const auto range = r::find_plateau(vs, es, ws, excluded, criteria);
  if (!range) return std::nullopt;
  auto mean = r::plateau_mean(vs, es, ws, excluded, *range, weighting, error);
  if (!mean) return std::nullopt;
  out.range = *range;
  out.mean = *mean;
  for (std::size_t i = range->first; i <= range->last; ++i)
    if (!ex[i]) out.used.push_back(out.steps[i]);
  return out;
}

Result<r::Mean> group_weighted_mean(const GroupItems& items, r::MeanErrorKind error) {
  std::vector<double> vs, es;
  for (const auto* it : items) {
    const r::UFloat& age = age_of(*it);
    vs.push_back(age.nominal());
    es.push_back(age.std_dev());
  }
  return r::weighted_mean(vs, es, error);
}

}  // namespace pychron::processing
