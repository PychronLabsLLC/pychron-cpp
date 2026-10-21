#include "pychron/processing/r_values.hpp"

#include <cmath>

#include "figure_common.hpp"
#include "pychron/processing/quantity.hpp"

namespace pychron::processing {

namespace {

GroupF group_f(const Quantity& f, int group, const std::string& name, const std::vector<const DatasetItem*>& items,
               const Options& o) {
  GroupF out;
  out.group = group;
  out.name = name;
  std::vector<double> vs, es;
  for (const auto* it : items) {
    if (!it->exclusion.included()) continue;
    const auto v = f.eval(*it->analysis);
    if (!v || !std::isfinite(v->value) || !std::isfinite(v->error)) continue;
    vs.push_back(v->value);
    es.push_back(v->error);
  }
  if (vs.empty()) {
    out.error = "no included analysis has F";
    return out;
  }
  auto m = detail::group_mean(vs, es, o);
  if (m)
    out.f = *m;
  else
    out.error = m.error().what;
  return out;
}

// F_j / F_i. The error is written so that it stays finite when F_j is zero:
// sqrt((s_j / F_i)^2 + (F_j s_i / F_i^2)^2) = |R| sqrt((s_i/F_i)^2 + (s_j/F_j)^2).
std::optional<RCell> ratio(const GroupF& den, const GroupF& num) {
  if (!den.f || !num.f || den.f->value == 0) return std::nullopt;
  const double fi = den.f->value, si = den.f->error, fj = num.f->value, sj = num.f->error;
  return RCell{fj / fi, std::hypot(sj / fi, fj * si / (fi * fi))};
}

}  // namespace

Result<std::vector<RValueMatrix>> r_value_matrices(const Dataset& dataset, const Options& ideogram_options) {
  auto f = Quantity::parse("F");
  if (!f) return fail(f.error());
  std::vector<RValueMatrix> out;
  for (int graph : dataset.graphs()) {
    RValueMatrix m;
    m.graph = graph;
    m.graph_name = dataset.graph_name(graph);
    for (const auto& [group, items] : dataset.groups_of_graph(graph))
      m.groups.push_back(group_f(*f, group, dataset.group_name(group), items, ideogram_options));
    const std::size_t n = m.groups.size();
    m.cells.assign(n, std::vector<std::optional<RCell>>(n));
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t j = 0; j < n; ++j)
        if (i != j) m.cells[i][j] = ratio(m.groups[i], m.groups[j]);
    out.push_back(std::move(m));
  }
  return out;
}

}  // namespace pychron::processing
