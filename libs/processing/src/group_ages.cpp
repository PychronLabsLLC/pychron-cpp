#include "pychron/processing/group_ages.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "pychron/reduction/arar_reduction.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::processing {

namespace r = reduction;

namespace {

double k39_of(const DatasetItem& it) {
  if (!it.analysis->arar) return std::numeric_limits<double>::quiet_NaN();
  return it.analysis->arar->f.interference.k39.nominal();
}

}  // namespace

bool has_ages(const DatasetItem& item) { return item.analysis->arar && item.analysis->arar->ages; }

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
    const r::UFloat& age = it->analysis->arar->ages->age;
    vs.push_back(age.nominal());
    es.push_back(age.std_dev());
    ws.push_back(k);
    out.steps.push_back(it);
  }
  const std::size_t n = out.steps.size();
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
    const r::UFloat& age = it->analysis->arar->ages->age;
    vs.push_back(age.nominal());
    es.push_back(age.std_dev());
  }
  return r::weighted_mean(vs, es, error);
}

}  // namespace pychron::processing
