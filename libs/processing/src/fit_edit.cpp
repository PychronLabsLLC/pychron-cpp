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

std::optional<StoredFit> stored_fit(const Analysis& a, SeriesKind kind, const std::string& key) {
  if (kind == SeriesKind::Signal) {
    const IsotopeData* iso = a.find_isotope(key);
    if (!iso) return std::nullopt;
    return StoredFit{iso->fit, iso->user_excluded, iso->intercept};
  }
  if (kind == SeriesKind::Baseline) {
    for (const auto& iso : a.isotopes)
      if (iso.detector == key) return StoredFit{iso.baseline_fit, iso.baseline_user_excluded, iso.baseline};
  }
  return std::nullopt;
}

Result<FitEditResult> apply_fit_edits(const Analysis& analysis, const RawData& raw, const std::vector<FitEdit>& edits) {
  auto copy = std::make_shared<Analysis>(analysis);
  FitEditResult result;
  for (const auto& e : edits) {
    const std::string what = e.kind == SeriesKind::Baseline ? e.key + " baseline" : e.key;
    if (e.kind != SeriesKind::Signal && e.kind != SeriesKind::Baseline)
      return fail(ErrorKind::Config, what + ": only signal and baseline fits can be edited");
    if (!stored_fit(*copy, e.kind, e.key))
      return fail(ErrorKind::Config, e.kind == SeriesKind::Baseline ? "no isotope on detector " + e.key : "no isotope " + e.key);
    const RawSeries* s = raw.find(e.kind, e.key);
    if (!s) return fail(ErrorKind::Config, what + ": no raw data to refit");
    auto fit = fit_series(*s, e.fit, e.user_excluded);
    if (!fit) return fail(ErrorKind::Config, what + ": " + fit.error().what);
    EditedFit ed;
    ed.kind = e.kind;
    ed.key = e.key;
    ed.value = Value{fit->intercept.value, fit->intercept.error};
    ed.fit = e.fit;
    ed.n_points = static_cast<int>(s->t.size());
    ed.n_used = static_cast<int>(fit->intercept.n_used);
    ed.user_excluded = e.user_excluded;
    for (auto& iso : copy->isotopes) {
      if (e.kind == SeriesKind::Signal && iso.key == e.key) {
        iso.intercept = ed.value;
        iso.fit = ed.fit;
        iso.n = ed.n_used;
        iso.user_excluded = ed.user_excluded;
      } else if (e.kind == SeriesKind::Baseline && iso.detector == e.key) {
        iso.baseline = ed.value;
        iso.baseline_fit = ed.fit;
        iso.baseline_user_excluded = ed.user_excluded;
      }
    }
    result.fits.push_back(std::move(ed));
  }
  result.analysis = std::move(copy);
  return result;
}

bool same_as_stored(const Analysis& a, const FitEdit& e) {
  const auto stored = stored_fit(a, e.kind, e.key);
  if (!stored || !stored->fit) return false;
  const auto& f = *stored->fit;
  return f.kind == e.fit.kind && f.error == e.fit.error && f.outliers.enabled == e.fit.outliers.enabled &&
         (!f.outliers.enabled ||
          (f.outliers.iterations == e.fit.outliers.iterations && f.outliers.std_devs == e.fit.outliers.std_devs)) &&
         stored->user_excluded == e.user_excluded;
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

std::string describe_fit_edits(const Analysis& before, const std::vector<EditedFit>& edits) {
  std::string out = "<ISOEVO>";
  bool first = true;
  for (const auto& e : edits) {
    out += first ? " " : ", ";
    first = false;
    out += e.kind == SeriesKind::Baseline ? e.key + " baseline" : e.key;
    const auto stored = stored_fit(before, e.kind, e.key);
    const std::optional<r::FitSpec> old = stored ? stored->fit : std::nullopt;
    const std::string was = old ? std::string(r::to_string(old->kind)) : "?";
    const std::string now(r::to_string(e.fit.kind));
    if (was != now) out += " " + was + " -> " + now;
    if (!old || old->error != e.fit.error) out += e.fit.error == r::ErrorType::Sd ? " SD" : " SEM";
    if (!old || old->outliers.enabled != e.fit.outliers.enabled || old->outliers.iterations != e.fit.outliers.iterations ||
        old->outliers.std_devs != e.fit.outliers.std_devs)
      out += e.fit.outliers.enabled ? " outliers " + std::to_string(e.fit.outliers.iterations) + "x" : " no outlier filter";
    if (!stored || stored->user_excluded != e.user_excluded) out += " " + std::to_string(e.user_excluded.size()) + " excluded";
  }
  return out;
}

}  // namespace pychron::processing
