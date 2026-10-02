#include "pychron/processing/recall.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "pychron/reduction/fits.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

std::optional<Value> val(const std::optional<r::UFloat>& u) {
  if (!u || !std::isfinite(u->nominal())) return std::nullopt;
  return Value{u->nominal(), u->std_dev()};
}

std::string utc(double t) {
  const auto tt = static_cast<std::time_t>(std::llround(t));
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
  return buf;
}

RecallRow text_row(std::string name, std::string text) {
  RecallRow row;
  row.name = std::move(name);
  row.text = std::move(text);
  return row;
}

RecallRow value_row(std::string name, std::optional<Value> v, std::string units = {}, std::string note = {}) {
  RecallRow row;
  row.name = std::move(name);
  row.value = v;
  row.units = std::move(units);
  row.note = std::move(note);
  if (!v) row.text = "—";
  return row;
}

std::string fit_name(const std::optional<r::FitSpec>& f) {
  if (!f) return {};
  std::string s(r::to_string(f->kind));
  if (f->outliers.enabled) s += "*";
  return s;
}

std::string join_ints(const std::vector<int>& v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + std::to_string(v[i]);
  return out;
}

}  // namespace

RecallModel make_recall_model(const ReducedAnalysis& ra) {
  RecallModel m;
  if (!ra.analysis) return m;
  const Analysis& a = *ra.analysis;
  m.title = a.runid + (a.sample.empty() ? "" : "  " + a.sample) + "  " + a.analysis_type;
  m.header = {utc(a.timestamp), a.mass_spectrometer, a.extract_device, "tag: " + a.tag};

  // Computed values.
  m.computed.title = "Computed";
  const auto& ar = ra.arar;
  if (ar) {
    if (ar->ages) {
      m.computed.rows.push_back(value_row("Age", val(ar->ages->age_w_j_err), "Ma", "with J error"));
      m.computed.rows.push_back(value_row("Age w/o J", val(ar->ages->age), "Ma", "analytical error only"));
      RecallRow wo;
      wo.name = "Age w/o J, irrad.";
      wo.value = Value{ar->ages->age.nominal(), ar->ages->age_err_wo_irrad};
      wo.units = "Ma";
      m.computed.rows.push_back(wo);
    } else {
      m.computed.rows.push_back(value_row("Age", std::nullopt, "Ma", "no J for this analysis"));
    }
    m.computed.rows.push_back(value_row("F (40Ar*/39ArK)", val(ar->f.f)));
    m.computed.rows.push_back(value_row("K/Ca", val(ar->kca)));
    m.computed.rows.push_back(value_row("K/Cl", val(ar->kcl)));
    m.computed.rows.push_back(value_row("40Ar*", val(ar->f.rad40), "fA"));
    m.computed.rows.push_back(value_row("%40Ar*", val(ar->f.radiogenic_yield), "%"));
    m.computed.rows.push_back(value_row("39ArK", val(ar->f.interference_corrected[r::index(r::ArgonIsotope::Ar39)]), "fA"));
    for (const auto d : ar->diagnostics) m.computed.rows.push_back(text_row("note", std::string(r::to_string(d))));
  } else {
    m.reduction_note = ra.reduction_error.empty() ? "no argon isotopes to reduce" : ra.reduction_error;
  }

  // Corrected ratios from the IC-corrected (and decay-corrected) stages.
  m.ratios.title = "Corrected ratios";
  auto stage = [&](const char* iso) -> std::optional<r::UFloat> {
    auto d = ra.stage(iso, Stage::DecayCorrected);
    return d ? d : ra.stage(iso, Stage::IcCorrected);
  };
  for (auto [n, d] : {std::pair{"Ar40", "Ar39"}, {"Ar40", "Ar36"}, {"Ar40", "Ar38"}, {"Ar38", "Ar39"}, {"Ar37", "Ar39"},
                      {"Ar36", "Ar39"}}) {
    auto num = stage(n), den = stage(d);
    if (!num || !den || den->nominal() == 0) continue;
    m.ratios.rows.push_back(value_row(std::string(n) + "/" + d, val(*num / *den)));
  }

  m.identity.title = "Identity";
  m.identity.rows = {text_row("Run id", a.runid),          text_row("UUID", a.uuid),
                     text_row("Analysis type", a.analysis_type), text_row("Sample", a.sample),
                     text_row("Material", a.material),     text_row("Project", a.project),
                     text_row("PI", a.principal_investigator),
                     text_row("Irradiation", a.irradiation + (a.level.empty() ? "" : " " + a.level) +
                                                 (a.position.empty() ? "" : " " + a.position)),
                     text_row("Load", a.load),             text_row("Analyst", a.analyst),
                     text_row("Comment", a.comment)};
  if (a.context.flux) {
    m.identity.rows.push_back(value_row("J", Value{a.context.flux->j.value, a.context.flux->j.error}));
  }

  m.extraction.title = "Extraction";
  const auto& ex = a.extraction;
  m.extraction.rows.push_back(text_row("Device", a.extract_device));
  if (ex.value) m.extraction.rows.push_back(value_row("Extract value", Value{*ex.value, 0}, ex.units));
  if (ex.duration) m.extraction.rows.push_back(value_row("Duration", Value{*ex.duration, 0}, "s"));
  if (ex.cleanup) m.extraction.rows.push_back(value_row("Cleanup", Value{*ex.cleanup, 0}, "s"));
  if (ex.weight) m.extraction.rows.push_back(value_row("Weight", Value{*ex.weight, 0}, "mg"));
  if (ex.beam_diameter) m.extraction.rows.push_back(value_row("Beam diameter", Value{*ex.beam_diameter, 0}));
  if (!ex.pattern.empty()) m.extraction.rows.push_back(text_row("Pattern", ex.pattern));
  if (!ex.positions.empty()) m.extraction.rows.push_back(text_row("Positions", join_ints(ex.positions)));

  m.spectrometer.title = "Spectrometer";
  for (const auto& [d, g] : a.gains) m.spectrometer.rows.push_back(value_row(d + " gain", Value{g, 0}));
  for (const auto& [d, g] : a.deflections) m.spectrometer.rows.push_back(value_row(d + " deflection", Value{g, 0}));
  for (const auto& [k, v] : a.source) m.spectrometer.rows.push_back(value_row(k, Value{v, 0}));
  for (const auto& pc : a.peak_centers) m.spectrometer.rows.push_back(value_row(pc.detector + " peak center", Value{pc.center, 0}));
  for (const auto& [k, v] : a.environmentals) m.spectrometer.rows.push_back(value_row(k, Value{v, 0}));

  for (std::size_t i = 0; i < a.isotopes.size(); ++i) {
    const auto& d = a.isotopes[i];
    RecallIsotope iso;
    iso.key = d.key;
    iso.isotope = d.isotope;
    iso.detector = d.detector;
    iso.fit = fit_name(d.fit);
    iso.baseline_fit = fit_name(d.baseline_fit);
    iso.blank_source = d.blank_source;
    iso.n = d.n;
    if (const auto* st = ra.find(d.key))
      for (const auto& [s, u] : st->values) iso.stages[s] = Value{u.nominal(), u.std_dev()};
    m.isotopes.push_back(std::move(iso));
  }

  if (ar && ar->ages) {
    // Error budget of the age with J error, by variable tag (legacy E20).
    const r::UFloat& age = ar->ages->age_w_j_err;
    const double var = age.variance();
    if (var > 0) {
      std::map<std::string, double> by;
      for (const auto& [t, sigma] : r::error_components(age)) by[std::string(r::tag_name(t))] += sigma * sigma / var * 100.0;
      for (const auto& [tag, pct] : by)
        if (pct >= 1e-6) m.error_budget.push_back({tag.empty() ? "other" : tag, pct});
    }
    std::sort(m.error_budget.begin(), m.error_budget.end(),
              [](const auto& x, const auto& y) { return x.percent > y.percent; });
  }
  return m;
}

Scene make_evolution_scene(const Analysis& a, const RawData& raw, SeriesKind kind, const std::vector<std::string>& keys) {
  Scene scene;
  scene.kind = "evolutions";
  Graph g;
  g.title = a.runid + "  " + std::string(to_string(kind));
  g.x.title = "Time (s)";
  bool any_x = false;
  double xmax = 0;
  int index = 0;
  for (const auto& s : raw.series) {
    if (s.kind != kind) continue;
    if (!keys.empty() && std::find(keys.begin(), keys.end(), s.key) == keys.end()) continue;
    Panel p;
    p.id = s.key;
    p.quantity = s.key;
    p.y.title = s.key + (s.detector.empty() || s.detector == s.key ? "" : " (" + s.detector + ")") + " (fA)";
    const Color color = palette_color(index++);
    PointLayer pts;
    pts.x = s.t;
    pts.y = s.v;
    pts.marker.size = 3;
    pts.marker.color = color;
    pts.excluded.assign(s.t.size(), false);
    pts.label = s.key;
    for (double t : s.t) {
      xmax = std::max(xmax, t);
      any_x = true;
    }

    const IsotopeData* iso = kind == SeriesKind::Signal ? a.find_isotope(s.key) : nullptr;
    if (iso && iso->fit && s.t.size() >= r::parameter_count(*iso->fit)) {
      r::Series series{s.t, s.v};
      auto fit = r::fit(series, *iso->fit);
      if (fit) {
        for (auto idx : fit->filtered_idx)
          if (idx < pts.excluded.size()) pts.excluded[idx] = true;
        // Refit at shifted origins: the fitted value and its error at each x.
        LineLayer line;
        line.style.color = color;
        line.style.width = 1.5;
        BandLayer band;
        Color fill = color;
        fill.a = 50;
        band.fill = fill;
        const double hi = s.t.empty() ? 0.0 : *std::max_element(s.t.begin(), s.t.end());
        constexpr int kSamples = 40;
        for (int k = 0; k < kSamples; ++k) {
          const double x = hi * k / (kSamples - 1);
          r::Series shifted{series.x, series.y};
          for (auto& t : shifted.x) t -= x;
          auto f = r::fit(shifted, *iso->fit);
          if (!f) continue;
          line.x.push_back(x);
          line.y.push_back(f->value);
          band.x.push_back(x);
          band.low.push_back(f->value - f->error);
          band.high.push_back(f->value + f->error);
        }
        if (!band.x.empty()) p.layers.emplace_back(std::move(band));
        if (!line.x.empty()) p.layers.emplace_back(std::move(line));
        TextLayer t;
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s  I(0) = %.6g ± %.3g  n %zu", std::string(r::to_string(iso->fit->kind)).c_str(),
                      fit->value, fit->error, fit->n_used);
        t.lines.push_back(buf);
        if (std::abs(fit->value - iso->intercept.value) > 1e-6 * std::max(1.0, std::abs(iso->intercept.value))) {
          std::snprintf(buf, sizeof buf, "stored %.6g ± %.3g", iso->intercept.value, iso->intercept.error);
          t.lines.push_back(buf);
        }
        t.corner = Corner::TopRight;  // signals decay from the left: keep t = 0 clear
        p.layers.emplace_back(std::move(t));
      }
    }
    pts.show_excluded = true;
    pts.excluded_marker = pts.marker;
    pts.excluded_marker.filled = false;
    pts.excluded_marker.color = Color{214, 39, 40, 255};
    p.layers.emplace_back(std::move(pts));
    g.panels.push_back(std::move(p));
  }
  if (any_x) {
    g.x.min = 0.0;
    g.x.max = xmax * 1.02 + 1e-9;
  }
  scene.style.legend = false;
  if (g.panels.empty()) scene.warnings.push_back("no " + std::string(to_string(kind)) + " data");
  scene.graphs.push_back(std::move(g));
  return scene;
}

}  // namespace pychron::processing
