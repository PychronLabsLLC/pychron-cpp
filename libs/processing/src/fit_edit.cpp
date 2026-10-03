#include "pychron/processing/fit_edit.hpp"

#include <algorithm>
#include <charconv>

namespace pychron::processing {

namespace r = pychron::reduction;

Result<SeriesFit> fit_series(const RawSeries& s, const r::FitSpec& spec, const std::vector<std::size_t>& user_excluded,
                             double origin) {
  if (s.t.size() != s.v.size()) return fail(ErrorKind::Config, s.key + ": time and value counts differ");
  std::vector<bool> drop(s.t.size(), false);
  for (auto i : user_excluded)
    if (i < drop.size()) drop[i] = true;
  r::Series series;
  std::vector<std::size_t> original;  // reduced index -> full index
  for (std::size_t i = 0; i < s.t.size(); ++i) {
    if (drop[i]) continue;
    series.x.push_back(s.t[i] - origin);
    series.y.push_back(s.v[i]);
    original.push_back(i);
  }
  auto fit = r::fit(series, spec);
  if (!fit) return fail(ErrorKind::Config, s.key + ": " + fit.error().what);
  SeriesFit out;
  out.intercept = std::move(*fit);
  for (auto idx : out.intercept.filtered_idx)
    if (idx < original.size()) out.outliers.push_back(original[idx]);
  return out;
}

Result<FitEditResult> apply_fit_edits(const Analysis& analysis, const RawData& raw, const std::vector<FitEdit>& edits) {
  auto copy = std::make_shared<Analysis>(analysis);
  FitEditResult result;
  for (const auto& e : edits) {
    auto it = std::find_if(copy->isotopes.begin(), copy->isotopes.end(), [&](const IsotopeData& d) { return d.key == e.key; });
    if (it == copy->isotopes.end()) return fail(ErrorKind::Config, "no isotope " + e.key);
    const RawSeries* s = raw.find(SeriesKind::Signal, e.key);
    if (!s) return fail(ErrorKind::Config, e.key + ": no raw signal to refit");
    auto fit = fit_series(*s, e.fit, e.user_excluded);
    if (!fit) return fail(fit.error());
    EditedIsotope ed;
    ed.key = e.key;
    ed.intercept = Value{fit->intercept.value, fit->intercept.error};
    ed.fit = e.fit;
    ed.n_points = static_cast<int>(s->t.size());
    ed.n_used = static_cast<int>(fit->intercept.n_used);
    ed.user_excluded = e.user_excluded;
    it->intercept = ed.intercept;
    it->fit = ed.fit;
    it->n = ed.n_used;
    it->user_excluded = ed.user_excluded;
    result.isotopes.push_back(std::move(ed));
  }
  result.analysis = std::move(copy);
  return result;
}

void toggle_index(std::vector<std::size_t>& v, std::size_t index) {
  auto it = std::lower_bound(v.begin(), v.end(), index);
  if (it != v.end() && *it == index) {
    v.erase(it);
  } else {
    v.insert(it, index);
  }
}

std::string evolution_ref(const std::string& key, std::size_t index) { return key + "#" + std::to_string(index); }

std::optional<std::pair<std::string, std::size_t>> parse_evolution_ref(const std::string& ref) {
  const auto hash = ref.rfind('#');
  if (hash == std::string::npos || hash == 0 || hash + 1 >= ref.size()) return std::nullopt;
  std::size_t index = 0;
  const char* first = ref.data() + hash + 1;
  const char* last = ref.data() + ref.size();
  const auto r = std::from_chars(first, last, index);
  if (r.ec != std::errc{} || r.ptr != last) return std::nullopt;
  return std::make_pair(ref.substr(0, hash), index);
}

std::string describe_fit_edits(const Analysis& before, const std::vector<EditedIsotope>& edits) {
  std::string out = "<ISOEVO>";
  bool first = true;
  for (const auto& e : edits) {
    out += first ? " " : ", ";
    first = false;
    out += e.key;
    const IsotopeData* old = before.find_isotope(e.key);
    const std::string was = old && old->fit ? std::string(r::to_string(old->fit->kind)) : "?";
    const std::string now(r::to_string(e.fit.kind));
    if (was != now) out += " " + was + " -> " + now;
    if (!old || !old->fit || old->fit->error != e.fit.error) out += e.fit.error == r::ErrorType::Sd ? " SD" : " SEM";
    if (!old || !old->fit || old->fit->outliers.enabled != e.fit.outliers.enabled ||
        old->fit->outliers.iterations != e.fit.outliers.iterations || old->fit->outliers.std_devs != e.fit.outliers.std_devs)
      out += e.fit.outliers.enabled ? " outliers " + std::to_string(e.fit.outliers.iterations) + "x" : " no outlier filter";
    if (!old || old->user_excluded != e.user_excluded) out += " " + std::to_string(e.user_excluded.size()) + " excluded";
  }
  return out;
}

}  // namespace pychron::processing
