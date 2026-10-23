#include "pychron/arar/r_values.hpp"

#include <array>
#include <charconv>
#include <cmath>

#include "figure_common.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/arar/report.hpp"

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

std::string fixed6(double v) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, 6);
  return {buf.data(), r.ptr};
}

std::string shortest(double v) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return {buf.data(), r.ptr};
}

std::string group_label(const GroupF& g, int nsigma) {
  return g.f ? g.name + " (" + r_value_text(g.f->value, g.f->error, nsigma) + ")" : g.name;
}

}  // namespace

std::string r_value_text(double value, double error_1s, int nsigma) {
  return fixed6(value) + " \xc2\xb1 " + fixed6(nsigma * error_1s);
}

std::string r_values_tsv(const std::vector<RValueMatrix>& matrices, int nsigma) {
  std::string out = "# " + std::string(kRValueNote) + "\n";
  bool first = true;
  for (const auto& m : matrices) {
    if (m.groups.size() < 2) continue;
    if (!first) out += '\n';
    first = false;
    out += m.graph_name + "\n";
    for (const auto& g : m.groups) out += "\t" + group_label(g, nsigma);
    out += '\n';
    for (std::size_t i = 0; i < m.groups.size(); ++i) {
      out += group_label(m.groups[i], nsigma);
      for (const auto& c : m.cells[i]) out += "\t" + (c ? r_value_text(c->r, c->r_err, nsigma) : std::string());
      out += '\n';
    }
  }
  return out;
}

std::string r_values_csv(const std::vector<RValueMatrix>& matrices, const Options& ideogram_options) {
  std::string out = "# " + std::string(kRValueNote) + "\n";
  out += "graph,numerator,denominator,f_num,f_num_err,f_den,f_den_err,r,r_err,mean,error_kind\n";
  const std::string tail =
      "," + csv_quote(ideogram_options.get_string("mean")) + "," + csv_quote(ideogram_options.get_string("error_kind"));
  for (const auto& m : matrices)
    for (std::size_t i = 0; i < m.groups.size(); ++i)
      for (std::size_t j = 0; j < m.groups.size(); ++j) {
        const auto& c = m.cells[i][j];
        if (!c) continue;
        const auto& num = m.groups[j];
        const auto& den = m.groups[i];
        out += csv_quote(m.graph_name) + "," + csv_quote(num.name) + "," + csv_quote(den.name) + "," +
               shortest(num.f->value) + "," + shortest(num.f->error) + "," + shortest(den.f->value) + "," +
               shortest(den.f->error) + "," + shortest(c->r) + "," + shortest(c->r_err) + tail + "\n";
      }
  return out;
}

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
