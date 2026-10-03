#include "pychron/processing/reference_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "pychron/processing/units.hpp"
#include "pychron/reduction/fits.hpp"
#include "pychron/reduction/stats.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;
using namespace detail;

namespace {

constexpr ReferenceFitKind kFitKinds[] = {
    ReferenceFitKind::Preceding, ReferenceFitKind::Succeeding,   ReferenceFitKind::BracketingAverage,
    ReferenceFitKind::BracketingInterpolate, ReferenceFitKind::Average, ReferenceFitKind::WeightedMean,
    ReferenceFitKind::Linear,    ReferenceFitKind::Parabolic,    ReferenceFitKind::Cubic,
    ReferenceFitKind::Exponential};

std::optional<r::FitKind> regression_kind(ReferenceFitKind k) {
  switch (k) {
    case ReferenceFitKind::Linear: return r::FitKind::Linear;
    case ReferenceFitKind::Parabolic: return r::FitKind::Parabolic;
    case ReferenceFitKind::Cubic: return r::FitKind::Cubic;
    case ReferenceFitKind::Exponential: return r::FitKind::Exponential;
    default: return std::nullopt;
  }
}

r::MeanErrorKind mean_error(ReferenceErrorKind e) {
  switch (e) {
    case ReferenceErrorKind::Sd: return r::MeanErrorKind::Sd;
    case ReferenceErrorKind::Msem: return r::MeanErrorKind::Msem;
    case ReferenceErrorKind::Sem: break;
  }
  return r::MeanErrorKind::Sem;
}

constexpr double kHour = 3600.0;

}  // namespace

std::string_view to_string(ReferenceFitKind kind) noexcept {
  switch (kind) {
    case ReferenceFitKind::Preceding: return "preceding";
    case ReferenceFitKind::Succeeding: return "succeeding";
    case ReferenceFitKind::BracketingAverage: return "bracketing_average";
    case ReferenceFitKind::BracketingInterpolate: return "bracketing_interpolate";
    case ReferenceFitKind::Average: return "average";
    case ReferenceFitKind::WeightedMean: return "weighted_mean";
    case ReferenceFitKind::Linear: return "linear";
    case ReferenceFitKind::Parabolic: return "parabolic";
    case ReferenceFitKind::Cubic: return "cubic";
    case ReferenceFitKind::Exponential: return "exponential";
  }
  return "";
}

std::optional<ReferenceFitKind> parse_reference_fit(std::string_view text) noexcept {
  for (auto k : kFitKinds)
    if (to_string(k) == text) return k;
  return std::nullopt;
}

bool is_interpolation(ReferenceFitKind kind) noexcept {
  return kind == ReferenceFitKind::Preceding || kind == ReferenceFitKind::Succeeding ||
         kind == ReferenceFitKind::BracketingAverage || kind == ReferenceFitKind::BracketingInterpolate;
}

std::string_view to_string(ReferenceErrorKind kind) noexcept {
  switch (kind) {
    case ReferenceErrorKind::Sem: return "SEM";
    case ReferenceErrorKind::Sd: return "SD";
    case ReferenceErrorKind::Msem: return "MSEM";
  }
  return "";
}

std::optional<ReferenceErrorKind> parse_reference_error(std::string_view text) noexcept {
  for (auto k : {ReferenceErrorKind::Sem, ReferenceErrorKind::Sd, ReferenceErrorKind::Msem})
    if (to_string(k) == text) return k;
  return std::nullopt;
}

std::string_view to_string(ReferenceFitTarget target) noexcept {
  return target == ReferenceFitTarget::Blanks ? "blanks" : "icfactors";
}

// ---------------------------------------------------------------- model

Result<ReferenceModel> ReferenceModel::make(std::vector<ReferencePoint> points, ReferenceFitKind kind,
                                            ReferenceErrorKind error) {
  ReferenceModel m;
  m.kind_ = kind;
  m.error_ = error;
  for (auto& p : points)
    if (!p.excluded && std::isfinite(p.t) && std::isfinite(p.value.value)) m.included_.push_back(std::move(p));
  std::stable_sort(m.included_.begin(), m.included_.end(),
                   [](const ReferencePoint& a, const ReferencePoint& b) { return a.t < b.t; });
  if (m.included_.empty()) return fail(ErrorKind::Config, "no references to fit");
  m.t0_ = m.included_.back().t;
  std::vector<double> v, e;
  for (const auto& p : m.included_) {
    v.push_back(p.value.value);
    e.push_back(p.value.error);
  }
  if (kind == ReferenceFitKind::Average || kind == ReferenceFitKind::WeightedMean) {
    auto mean = kind == ReferenceFitKind::Average ? r::arithmetic_mean(v, e, mean_error(error))
                                                  : r::weighted_mean(v, e, mean_error(error));
    if (!mean) return fail(ErrorKind::Config, std::string(to_string(kind)) + ": " + mean.error().what);
    m.constant_ = Value{mean->value, mean->error};
    if (mean->n > 1) m.mswd_ = mean->mswd;
  } else if (auto rk = regression_kind(kind)) {
    r::FitSpec spec;
    spec.kind = *rk;
    if (m.included_.size() < r::parameter_count(spec))
      return fail(ErrorKind::Config, std::string(to_string(kind)) + " needs " + std::to_string(r::parameter_count(spec)) +
                                         " references, has " + std::to_string(m.included_.size()));
    auto probe = m.at(m.t0_);
    if (!probe) return fail(probe.error());
  }
  return m;
}

Result<Value> ReferenceModel::at(double t) const {
  if (constant_) return *constant_;
  const auto& p = included_;
  if (is_interpolation(kind_)) {
    // Last point at or before t, first at or after (clamped at the ends).
    std::size_t hi = static_cast<std::size_t>(
        std::lower_bound(p.begin(), p.end(), t, [](const ReferencePoint& a, double x) { return a.t < x; }) - p.begin());
    std::size_t lo = hi;
    if (hi == p.size() || p[hi].t > t) lo = hi == 0 ? 0 : hi - 1;
    if (hi == p.size()) hi = p.size() - 1;
    const auto& L = p[lo];
    const auto& H = p[hi];
    switch (kind_) {
      case ReferenceFitKind::Preceding: return (L.t <= t ? L : p.front()).value;
      case ReferenceFitKind::Succeeding: return (H.t >= t ? H : p.back()).value;
      case ReferenceFitKind::BracketingAverage:
        if (lo == hi) return L.value;
        return Value{(L.value.value + H.value.value) / 2.0, std::hypot(L.value.error, H.value.error) / 2.0};
      case ReferenceFitKind::BracketingInterpolate: {
        if (lo == hi || H.t == L.t) return L.value;
        const double f = std::clamp((t - L.t) / (H.t - L.t), 0.0, 1.0);
        return Value{L.value.value + f * (H.value.value - L.value.value),
                     std::hypot((1.0 - f) * L.value.error, f * H.value.error)};
      }
      default: break;
    }
  }
  const auto rk = regression_kind(kind_);
  if (!rk) return fail(ErrorKind::Config, "unsupported fit");
  // Hours relative to `t`, so the intercept is the value at t.
  r::Series s;
  for (const auto& q : p) {
    s.x.push_back((q.t - t) / kHour);
    s.y.push_back(q.value.value);
  }
  r::FitSpec spec;
  spec.kind = *rk;
  spec.error = error_ == ReferenceErrorKind::Sd ? r::ErrorType::Sd : r::ErrorType::Sem;
  auto f = r::fit(s, spec);
  if (!f) return fail(ErrorKind::Config, std::string(to_string(kind_)) + ": " + f.error().what);
  return Value{f->value, f->error};
}

// ---------------------------------------------------------------- fit sets

std::string ReferenceFitSet::message() const {
  std::string out = target == ReferenceFitTarget::Blanks ? "<BLANKS> fits=" : "<ICFactor> fits=";
  std::set<std::string> seen;
  bool first = true;
  for (const auto& a : analyses)
    for (const auto& row : a.rows) {
      if (!seen.insert(row.key).second) continue;
      out += (first ? "" : ",") + row.key + "(" + std::string(to_string(row.fit)) + ")";
      first = false;
    }
  return out;
}

// ---------------------------------------------------------------- schemas

namespace {

std::vector<std::string> fit_choices() {
  std::vector<std::string> out;
  for (auto k : kFitKinds) out.emplace_back(to_string(k));
  return out;
}

std::vector<FieldSpec> common_fields() {
  return {integer("nsigma", "Error bars (σ)", "Display", 1, 1, 3),
          boolean("show_current", "Show current values", "Display", true,
                  "The unknowns' stored values beside the predicted ones")};
}

SchemaPtr make_blank_schema() {
  auto row = make_schema("figure.blank_fit.isotope", "Isotope",
                         {text("isotope", "Isotope", "Isotope", "Ar40"),
                          choice("fit", "Fit", "Isotope", fit_choices(), "preceding"),
                          choice("error", "Error", "Isotope", {"SEM", "SD", "MSEM"}, "SEM")});
  ListSpec list;
  list.key = "isotopes";
  list.label = "Isotopes";
  list.section = "Isotopes";
  list.row = row;
  list.min_rows = 1;
  list.max_rows = 16;
  for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"})
    list.default_rows_toml.push_back(std::string("isotope = \"") + iso + "\"");
  auto s = std::const_pointer_cast<Schema>(make_schema("figure.blank_fit", "Blanks", common_fields(), {list}));
  s->factory_presets = {{"Default", ""}};
  return s;
}

SchemaPtr make_icfactor_schema() {
  auto row = make_schema("figure.icfactor_fit.ratio", "Ratio",
                         {text("numerator", "Numerator detector", "Ratio", "H1"),
                          text("denominator", "Denominator detector", "Ratio", "CDD"),
                          number("standard_ratio", "Standard ratio", "Ratio", 295.5, 1e-9, 1e12),
                          choice("fit", "Fit", "Ratio", fit_choices(), "average"),
                          choice("error", "Error", "Ratio", {"SEM", "SD", "MSEM"}, "SEM")});
  ListSpec list;
  list.key = "ratios";
  list.label = "Ratios";
  list.section = "Ratios";
  list.row = row;
  list.min_rows = 1;
  list.max_rows = 16;
  list.default_rows_toml = {"numerator = \"H1\"\ndenominator = \"CDD\""};
  auto s = std::const_pointer_cast<Schema>(make_schema("figure.icfactor_fit", "IC factors", common_fields(), {list}));
  s->factory_presets = {{"Default", ""}};
  return s;
}

// The reduced value of the first isotope on `detector` at `stage`.
std::optional<r::UFloat> on_detector(const ReducedAnalysis& ra, const std::string& detector, Stage stage) {
  for (const auto& iso : ra.analysis->isotopes)
    if (iso.detector == detector) return ra.stage(iso.key, stage);
  return std::nullopt;
}

const IsotopeData* isotope_on(const Analysis& a, const std::string& detector) {
  for (const auto& iso : a.isotopes)
    if (iso.detector == detector) return &iso;
  return nullptr;
}

std::optional<Value> finite(const std::optional<r::UFloat>& u) {
  if (!u || !std::isfinite(u->nominal())) return std::nullopt;
  return Value{u->nominal(), u->std_dev()};
}

struct RowSpec {
  std::string title;  // panel title and fit-set message key source
  std::string isotope;                  // blanks
  std::string numerator, denominator;   // IC factors
  double standard_ratio = 1.0;
  ReferenceFitKind fit = ReferenceFitKind::Average;
  ReferenceErrorKind error = ReferenceErrorKind::Sem;
};

std::vector<RowSpec> row_specs(ReferenceFitTarget target, const Options& o) {
  std::vector<RowSpec> out;
  for (const auto& row : o.rows(target == ReferenceFitTarget::Blanks ? "isotopes" : "ratios")) {
    RowSpec s;
    s.fit = parse_reference_fit(row.get_string("fit")).value_or(ReferenceFitKind::Average);
    s.error = parse_reference_error(row.get_string("error")).value_or(ReferenceErrorKind::Sem);
    if (target == ReferenceFitTarget::Blanks) {
      s.isotope = row.get_string("isotope");
      s.title = s.isotope;
    } else {
      s.numerator = row.get_string("numerator");
      s.denominator = row.get_string("denominator");
      s.standard_ratio = row.get_double("standard_ratio");
      s.title = s.numerator + "/" + s.denominator;
    }
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace

const SchemaPtr& blank_fit_schema() {
  static const SchemaPtr s = make_blank_schema();
  return s;
}

const SchemaPtr& icfactor_fit_schema() {
  static const SchemaPtr s = make_icfactor_schema();
  return s;
}

// ---------------------------------------------------------------- figure

Result<ReferenceFigure> build_reference_figure(ReferenceFitTarget target, const Dataset& unknowns,
                                               const Dataset& references, const Options& o) {
  ReferenceFigure fig;
  fig.fits.target = target;
  fig.scene.kind = target == ReferenceFitTarget::Blanks ? "blank_fit" : "icfactor_fit";
  const double nsigma = static_cast<double>(o.get_int("nsigma"));
  const bool show_current = o.get_bool("show_current");
  const bool blanks = target == ReferenceFitTarget::Blanks;

  // Time zero: the newest run among references and unknowns.
  double tmax = -1e300;
  for (const auto* d : {&unknowns, &references})
    for (const auto& item : d->items()) tmax = std::max(tmax, item.analysis->analysis->timestamp);
  if (tmax == -1e300) tmax = 0.0;
  auto hours = [&](double t) { return (t - tmax) / kHour; };

  std::vector<const DatasetItem*> fitted;
  for (const auto& item : unknowns.items())
    if (item.exclusion.included()) fitted.push_back(&item);
  std::map<std::string, AnalysisReferenceFits> by_uuid;

  Graph g;
  g.title = blanks ? "Blanks" : "IC factors";
  g.x.title = "Hours (0 = newest run)";
  double xmin = 0.0;
  int index = 0;
  for (const auto& spec : row_specs(target, o)) {
    const Color color = palette_color(index);
    Panel p;
    p.id = "p" + std::to_string(index++);
    p.quantity = spec.title;
    p.y.title = blanks ? spec.isotope + " blank (fA)" : spec.title + " (N/D) / standard";

    // References.
    std::vector<ReferencePoint> points;
    std::vector<ReferenceUse> uses;
    PointLayer refs;
    refs.marker.color = color;
    refs.marker.size = 5;
    refs.excluded_marker = refs.marker;
    refs.excluded_marker.filled = false;
    refs.label = "references";
    for (const auto& item : references.items()) {
      const ReducedAnalysis& ra = *item.analysis;
      std::optional<Value> v;
      if (blanks) {
        v = finite(ra.stage(spec.isotope, Stage::BaselineCorrected));
      } else {
        const auto n = on_detector(ra, spec.numerator, Stage::BlankCorrected);
        const auto d = on_detector(ra, spec.denominator, Stage::BlankCorrected);
        if (n && d && d->nominal() != 0.0) v = finite(*n / *d / spec.standard_ratio);
      }
      if (!v) continue;
      const Analysis& a = *ra.analysis;
      ReferencePoint pt{a.timestamp, *v, a.uuid, a.runid, item.exclusion.excluded()};
      uses.push_back({a.uuid, a.runid, pt.excluded});
      refs.x.push_back(hours(pt.t));
      refs.y.push_back(v->value);
      refs.y_err.push_back(v->error * nsigma);
      refs.refs.push_back(PointRef{a.uuid});
      refs.excluded.push_back(pt.excluded);
      refs.tooltips.push_back(a.runid + "  " + a.analysis_type);
      xmin = std::min(xmin, refs.x.back());
      points.push_back(std::move(pt));
    }

    auto model = ReferenceModel::make(points, spec.fit, spec.error);
    if (!model) {
      const std::string w = spec.title + ": " + model.error().what;
      fig.scene.warnings.push_back(w);
      fig.fits.warnings.push_back(w);
    }

    // Unknowns: current and predicted values.
    PointLayer current, predicted;
    current.marker.shape = MarkerShape::Square;
    current.marker.color = Color{140, 140, 140, 255};
    current.marker.filled = false;
    current.label = "current";
    predicted.marker.shape = MarkerShape::Diamond;
    predicted.marker.size = 7;
    predicted.marker.color = Color{214, 39, 40, 255};
    predicted.label = "predicted";
    for (const DatasetItem* item : fitted) {
      const Analysis& a = *item->analysis->analysis;
      std::vector<std::pair<std::string, Value>> keys;  // stored row key, current value
      if (blanks) {
        for (const auto& iso : a.isotopes)
          if (iso.isotope == spec.isotope || iso.key == spec.isotope) keys.emplace_back(iso.key, iso.blank);
      } else if (const IsotopeData* iso = isotope_on(a, spec.denominator)) {
        keys.emplace_back(spec.denominator, iso->ic_factor);
      }
      if (keys.empty()) continue;
      const double x = hours(a.timestamp);
      xmin = std::min(xmin, x);
      if (show_current) {
        current.x.push_back(x);
        current.y.push_back(keys.front().second.value);
        current.y_err.push_back(keys.front().second.error * nsigma);
        current.refs.push_back(PointRef{});
        current.tooltips.push_back(a.runid + " current");
      }
      if (!model) continue;
      auto v = model->at(a.timestamp);
      if (!v) {
        fig.fits.warnings.push_back(a.runid + " " + spec.title + ": " + v.error().what);
        continue;
      }
      predicted.x.push_back(x);
      predicted.y.push_back(v->value);
      predicted.y_err.push_back(v->error * nsigma);
      predicted.refs.push_back(PointRef{});
      predicted.tooltips.push_back(a.runid + " predicted");
      auto& entry = by_uuid[a.uuid];
      entry.uuid = a.uuid;
      entry.runid = a.runid;
      entry.heads = a.heads;
      for (const auto& [key, _] : keys) {
        ReferenceRowFit row;
        row.key = key;
        row.value = *v;
        row.fit = spec.fit;
        row.error = spec.error;
        if (!blanks) {
          row.reference_detector = spec.numerator;
          row.standard_ratio = spec.standard_ratio;
        }
        row.references = uses;
        entry.rows.push_back(std::move(row));
      }
    }
    current.excluded.assign(current.x.size(), false);
    predicted.excluded.assign(predicted.x.size(), false);

    if (model) {
      LineLayer line;
      line.style.color = color;
      line.style.width = 1.5;
      line.label = std::string(to_string(spec.fit));
      BandLayer band;
      band.fill = color;
      band.fill.a = 45;
      constexpr int kSamples = 200;
      const double lo = std::min(xmin, 0.0), hi = 0.0;
      for (int k = 0; k < kSamples; ++k) {
        const double x = lo + (hi - lo) * k / (kSamples - 1);
        auto v = model->at(tmax + x * kHour);
        if (!v) continue;
        line.x.push_back(x);
        line.y.push_back(v->value);
        band.x.push_back(x);
        band.low.push_back(v->value - v->error * nsigma);
        band.high.push_back(v->value + v->error * nsigma);
      }
      if (!band.x.empty()) p.layers.emplace_back(std::move(band));
      if (!line.x.empty()) p.layers.emplace_back(std::move(line));
      TextLayer t;
      char buf[160];
      std::snprintf(buf, sizeof buf, "%s %s  n %zu of %zu", std::string(to_string(spec.fit)).c_str(),
                    is_interpolation(spec.fit) ? "" : std::string(to_string(spec.error)).c_str(),
                    model->included().size(), points.size());
      t.lines.push_back(buf);
      if (model->mswd()) {
        std::snprintf(buf, sizeof buf, "MSWD %.3g", *model->mswd());
        t.lines.push_back(buf);
      }
      t.corner = Corner::TopLeft;
      p.layers.emplace_back(std::move(t));
    }
    p.layers.emplace_back(std::move(refs));
    if (!current.x.empty()) p.layers.emplace_back(std::move(current));
    if (!predicted.x.empty()) p.layers.emplace_back(std::move(predicted));
    g.panels.push_back(std::move(p));
  }
  if (g.panels.empty()) return fail(ErrorKind::Config, "no rows to fit");
  const double pad = std::max(0.5, -xmin * 0.03);
  g.x.min = xmin - pad;
  g.x.max = pad;
  fig.scene.graphs.push_back(std::move(g));
  fig.scene.style.legend = true;
  if (references.empty()) fig.scene.warnings.push_back("no references");
  for (const DatasetItem* item : fitted)
    if (auto it = by_uuid.find(item->analysis->analysis->uuid); it != by_uuid.end())
      fig.fits.analyses.push_back(std::move(it->second));
  return fig;
}

// ---------------------------------------------------------------- finding references

std::vector<std::string> default_reference_types(ReferenceFitTarget target) {
  if (target == ReferenceFitTarget::IcFactors) return {"air"};
  return {"blank_unknown", "blank_air", "blank_cocktail", "blank"};
}

Result<std::vector<std::string>> find_references(IAnalysisSource& source, const std::vector<AnalysisPtr>& unknowns,
                                                 const ReferenceQuery& query) {
  if (unknowns.empty()) return std::vector<std::string>{};
  // Windows around each unknown, merged where they overlap.
  std::vector<std::pair<double, double>> windows;
  std::set<std::string> spectrometers, devices, skip;
  const double h = std::max(0.0, query.hours) * kHour;
  for (const auto& a : unknowns) {
    windows.emplace_back(a->timestamp - h, a->timestamp + h);
    if (!a->mass_spectrometer.empty()) spectrometers.insert(a->mass_spectrometer);
    if (!a->extract_device.empty()) devices.insert(a->extract_device);
    skip.insert(a->uuid);
  }
  std::sort(windows.begin(), windows.end());
  std::vector<std::pair<double, double>> merged;
  for (const auto& w : windows) {
    if (!merged.empty() && w.first <= merged.back().second) {
      merged.back().second = std::max(merged.back().second, w.second);
    } else {
      merged.push_back(w);
    }
  }
  std::vector<AnalysisSummary> found;
  std::set<std::string> seen;
  for (const auto& [from, to] : merged) {
    BrowseQuery q;
    q.analysis_types = query.analysis_types;
    if (query.same_mass_spectrometer) q.mass_spectrometers.assign(spectrometers.begin(), spectrometers.end());
    if (query.same_extract_device) q.extract_devices.assign(devices.begin(), devices.end());
    q.from = from;
    q.to = to;
    q.exclude_tags = {"invalid"};
    q.limit = 500;
    while (true) {
      auto page = source.browse(q);
      if (!page) return fail(page.error());
      for (auto& row : page->rows)
        if (!skip.count(row.uuid) && seen.insert(row.uuid).second) found.push_back(std::move(row));
      if (!page->next || static_cast<int>(found.size()) >= query.limit) break;
      q.after = page->next;
    }
    if (static_cast<int>(found.size()) >= query.limit) break;
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const AnalysisSummary& a, const AnalysisSummary& b) { return a.timestamp > b.timestamp; });
  if (static_cast<int>(found.size()) > query.limit) found.resize(static_cast<std::size_t>(query.limit));
  std::vector<std::string> out;
  for (const auto& s : found) out.push_back(s.uuid);
  return out;
}

// ---------------------------------------------------------------- units

namespace {

class ReferenceFitUnit final : public Unit {
 public:
  explicit ReferenceFitUnit(ReferenceFitTarget target) : target_(target) {}
  std::string_view kind() const override { return target_ == ReferenceFitTarget::Blanks ? "blank_fit" : "icfactor_fit"; }
  std::string_view title() const override { return target_ == ReferenceFitTarget::Blanks ? "Blanks" : "IC factors"; }
  const SchemaPtr& schema() const override {
    return target_ == ReferenceFitTarget::Blanks ? blank_fit_schema() : icfactor_fit_schema();
  }
  std::vector<PortSpec> inputs() const override {
    return {{"unknowns", PortType::Dataset}, {"references", PortType::Dataset}};
  }
  std::vector<PortSpec> outputs() const override {
    return {{"figure", PortType::Scene}, {"fits", PortType::ReferenceFits}};
  }
  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext& ctx) const override {
    auto fig = build_reference_figure(target_, *std::get<DatasetPtr>(in.at(0)), *std::get<DatasetPtr>(in.at(1)), o);
    if (!fig) return fail(fig.error());
    for (const auto& w : fig->fits.warnings) ctx.diagnostics.push_back(w);
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(fig->scene))),
                                  PortValue(std::make_shared<const ReferenceFitSet>(std::move(fig->fits)))};
  }

 private:
  ReferenceFitTarget target_;
};

}  // namespace

std::unique_ptr<Unit> make_reference_fit_unit(ReferenceFitTarget target) {
  return std::make_unique<ReferenceFitUnit>(target);
}

}  // namespace pychron::processing
