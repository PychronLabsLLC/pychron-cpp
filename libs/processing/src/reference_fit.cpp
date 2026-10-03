#include "pychron/processing/reference_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <random>
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


constexpr double kHour = 3600.0;

// Legacy masses (arar_age.py set_beta).
constexpr double kMass40 = 39.9624, kMass36 = 35.9675;
constexpr std::pair<const char*, double> kSourceMasses[] = {
    {"Ar36", 35.9675}, {"Ar37", 36.9668}, {"Ar38", 37.9627}, {"Ar39", 38.964}};

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
    case ReferenceErrorKind::Ci: return "CI";
    case ReferenceErrorKind::MonteCarlo: return "MC";
  }
  return "";
}

std::optional<ReferenceErrorKind> parse_reference_error(std::string_view text) noexcept {
  for (auto k : {ReferenceErrorKind::Sem, ReferenceErrorKind::Sd, ReferenceErrorKind::Msem, ReferenceErrorKind::Ci,
                 ReferenceErrorKind::MonteCarlo})
    if (to_string(k) == text) return k;
  return std::nullopt;
}

std::string_view to_string(ReferenceFitTarget target) noexcept {
  return target == ReferenceFitTarget::Blanks ? "blanks" : "icfactors";
}

// ---------------------------------------------------------------- model

namespace {

// Deterministic normal deviates: Box-Muller over mt19937_64, whose output is
// fixed by the standard (std::normal_distribution is not, across libraries).
class Normal {
 public:
  explicit Normal(std::uint64_t seed) : rng_(seed) {}
  double operator()() {
    if (spare_) {
      const double v = *spare_;
      spare_.reset();
      return v;
    }
    const double u1 = (static_cast<double>(rng_() >> 11) + 1.0) * 0x1.0p-53;  // (0, 1]
    const double u2 = static_cast<double>(rng_() >> 11) * 0x1.0p-53;          // [0, 1)
    const double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(2.0 * std::numbers::pi * u2);
    return r * std::cos(2.0 * std::numbers::pi * u2);
  }

 private:
  std::mt19937_64 rng_;
  std::optional<double> spare_;
};

constexpr std::uint64_t kMonteCarloSeed = 0x5eed2026;

int degree_of(ReferenceFitKind k) {
  switch (k) {
    case ReferenceFitKind::Linear: return 1;
    case ReferenceFitKind::Parabolic: return 2;
    case ReferenceFitKind::Cubic: return 3;
    default: return 0;
  }
}

// Weighted least squares polynomial of `degree`: coefficients and
// (X'WX)^-1, row major. nullopt when singular.
struct Wls {
  std::vector<double> beta, cov;
};
std::optional<Wls> wls(const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& w,
                       int degree) {
  const int p = degree + 1;
  std::vector<double> a(static_cast<std::size_t>(p * p), 0.0), b(static_cast<std::size_t>(p), 0.0);
  for (std::size_t i = 0; i < x.size(); ++i) {
    std::vector<double> pw(static_cast<std::size_t>(p), 1.0);
    for (int k = 1; k < p; ++k) pw[static_cast<std::size_t>(k)] = pw[static_cast<std::size_t>(k - 1)] * x[i];
    for (int r = 0; r < p; ++r) {
      b[static_cast<std::size_t>(r)] += w[i] * pw[static_cast<std::size_t>(r)] * y[i];
      for (int c = 0; c < p; ++c)
        a[static_cast<std::size_t>(r * p + c)] += w[i] * pw[static_cast<std::size_t>(r)] * pw[static_cast<std::size_t>(c)];
    }
  }
  // Gauss-Jordan on [A | I | b] with partial pivoting.
  std::vector<double> inv(static_cast<std::size_t>(p * p), 0.0);
  for (int i = 0; i < p; ++i) inv[static_cast<std::size_t>(i * p + i)] = 1.0;
  auto at = [&](std::vector<double>& m, int r, int c) -> double& { return m[static_cast<std::size_t>(r * p + c)]; };
  for (int col = 0; col < p; ++col) {
    int piv = col;
    for (int r = col + 1; r < p; ++r)
      if (std::abs(at(a, r, col)) > std::abs(at(a, piv, col))) piv = r;
    if (std::abs(at(a, piv, col)) < 1e-300) return std::nullopt;
    if (piv != col) {
      for (int c = 0; c < p; ++c) {
        std::swap(at(a, piv, c), at(a, col, c));
        std::swap(at(inv, piv, c), at(inv, col, c));
      }
      std::swap(b[static_cast<std::size_t>(piv)], b[static_cast<std::size_t>(col)]);
    }
    const double d = at(a, col, col);
    for (int c = 0; c < p; ++c) {
      at(a, col, c) /= d;
      at(inv, col, c) /= d;
    }
    b[static_cast<std::size_t>(col)] /= d;
    for (int r = 0; r < p; ++r) {
      if (r == col) continue;
      const double f = at(a, r, col);
      if (f == 0.0) continue;
      for (int c = 0; c < p; ++c) {
        at(a, r, c) -= f * at(a, col, c);
        at(inv, r, c) -= f * at(inv, col, c);
      }
      b[static_cast<std::size_t>(r)] -= f * b[static_cast<std::size_t>(col)];
    }
  }
  return Wls{std::move(b), std::move(inv)};
}

double poly(const std::vector<double>& c, double x) {
  double v = 0.0;
  for (auto it = c.rbegin(); it != c.rend(); ++it) v = v * x + *it;
  return v;
}

double sd_of(const std::vector<double>& v) {
  if (v.size() < 2) return 0.0;
  double m = 0.0;
  for (double x : v) m += x;
  m /= static_cast<double>(v.size());
  double ss = 0.0;
  for (double x : v) ss += (x - m) * (x - m);
  return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

// t(0.975, dof), or 1 without degrees of freedom.
double t95(std::size_t n, std::size_t p) {
  return n > p ? r::student_t_quantile(0.975, static_cast<double>(n - p)) : 1.0;
}

}  // namespace

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
  const std::size_t n = m.included_.size();
  std::vector<double> v, e, x;
  for (const auto& p : m.included_) {
    v.push_back(p.value.value);
    e.push_back(p.value.error);
    x.push_back((p.t - m.t0_) / kHour);
  }
  const bool all_errors =
      std::all_of(e.begin(), e.end(), [](double s) { return s > 0.0 && std::isfinite(s); });
  Normal normal(kMonteCarloSeed);
  auto perturbed = [&] {
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = v[i] + e[i] * normal();
    return out;
  };

  if (kind == ReferenceFitKind::Average || kind == ReferenceFitKind::WeightedMean) {
    const bool weighted = kind == ReferenceFitKind::WeightedMean;
    auto mean_of = [&](const std::vector<double>& vals, r::MeanErrorKind mk) {
      return weighted ? r::weighted_mean(vals, e, mk) : r::arithmetic_mean(vals, e, mk);
    };
    const auto base = error == ReferenceErrorKind::Sd ? r::MeanErrorKind::Sd
                      : error == ReferenceErrorKind::Sem ? r::MeanErrorKind::Sem
                                                         : r::MeanErrorKind::Msem;
    auto mean = mean_of(v, base);
    if (!mean) return fail(ErrorKind::Config, std::string(to_string(kind)) + ": " + mean.error().what);
    double err = mean->error;
    if (error == ReferenceErrorKind::Ci) err = t95(mean->n, 1) * mean->error;  // mean->error is MSEM here
    if (error == ReferenceErrorKind::MonteCarlo) {
      std::vector<double> means;
      for (int k = 0; k < kMonteCarloTrials; ++k)
        if (auto t = mean_of(perturbed(), r::MeanErrorKind::Sem)) means.push_back(t->value);
      err = sd_of(means);
    }
    m.constant_ = Value{mean->value, err};
    if (mean->n > 1) m.mswd_ = mean->mswd;
    return m;
  }

  const auto rk = regression_kind(kind);
  if (!rk) return m;  // interpolations need nothing more
  r::FitSpec spec;
  spec.kind = *rk;
  const std::size_t params = r::parameter_count(spec);
  if (n < params)
    return fail(ErrorKind::Config, std::string(to_string(kind)) + " needs " + std::to_string(params) +
                                       " references, has " + std::to_string(n));
  const int degree = degree_of(kind);
  if (degree > 0 && all_errors) {
    std::vector<double> w;
    for (double s : e) w.push_back(1.0 / (s * s));
    auto fit = wls(x, v, w, degree);
    if (!fit) return fail(ErrorKind::Config, std::string(to_string(kind)) + ": singular fit");
    m.weighted_ = true;
    m.beta_ = std::move(fit->beta);
    m.cov_ = std::move(fit->cov);
    double chi2 = 0.0, ss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double res = v[i] - poly(m.beta_, x[i]);
      chi2 += w[i] * res * res;
      ss += res * res;
    }
    if (n > params) {
      m.mswd_ = chi2 / static_cast<double>(n - params);
      m.residual_variance_ = ss / static_cast<double>(n - params);
    }
    if (error == ReferenceErrorKind::MonteCarlo)
      for (int k = 0; k < kMonteCarloTrials; ++k)
        if (auto t = wls(x, perturbed(), w, degree)) m.trials_.push_back(std::move(t->beta));
  } else {
    auto probe = m.at(m.t0_);
    if (!probe) return fail(probe.error());
    if (error == ReferenceErrorKind::MonteCarlo)
      for (int k = 0; k < kMonteCarloTrials; ++k) {
        auto t = r::fit(r::Series{x, perturbed()}, spec);
        if (t) m.trials_.push_back(t->params);
      }
  }
  return m;
}

Result<Value> ReferenceModel::regression_at(double t) const {
  const double h = (t - t0_) / kHour;
  const std::size_t n = included_.size();
  r::FitSpec spec;
  spec.kind = *regression_kind(kind_);
  const std::size_t params = r::parameter_count(spec);
  auto monte_carlo = [&](double value) -> Value {
    std::vector<double> preds;
    for (const auto& c : trials_) {
      r::Intercept curve;
      curve.kind = spec.kind;
      curve.params = c;
      const double y = weighted_ ? poly(c, h) : r::predict(curve, h);
      if (std::isfinite(y)) preds.push_back(y);
    }
    return Value{value, sd_of(preds)};
  };
  if (weighted_) {
    const int p = static_cast<int>(beta_.size());
    std::vector<double> xk(static_cast<std::size_t>(p), 1.0);
    for (int k = 1; k < p; ++k) xk[static_cast<std::size_t>(k)] = xk[static_cast<std::size_t>(k - 1)] * h;
    double var = 0.0;
    for (int r1 = 0; r1 < p; ++r1)
      for (int c = 0; c < p; ++c)
        var += xk[static_cast<std::size_t>(r1)] * cov_[static_cast<std::size_t>(r1 * p + c)] * xk[static_cast<std::size_t>(c)];
    const double sem = std::sqrt(std::max(0.0, var));
    const double value = poly(beta_, h);
    const double msem = sem * std::sqrt(std::max(1.0, mswd_.value_or(1.0)));
    switch (error_) {
      case ReferenceErrorKind::Sem: return Value{value, sem};
      case ReferenceErrorKind::Msem: return Value{value, msem};
      case ReferenceErrorKind::Sd: return Value{value, std::sqrt(sem * sem + residual_variance_)};
      case ReferenceErrorKind::Ci: return Value{value, t95(n, params) * msem};
      case ReferenceErrorKind::MonteCarlo: return monte_carlo(value);
    }
    return Value{value, sem};
  }
  // Unweighted: refit with time zero at t, so the intercept is the value at t.
  r::Series s;
  for (const auto& q : included_) {
    s.x.push_back((q.t - t) / kHour);
    s.y.push_back(q.value.value);
  }
  spec.error = error_ == ReferenceErrorKind::Sd ? r::ErrorType::Sd : r::ErrorType::Sem;
  auto f = r::fit(s, spec);
  if (!f) return fail(ErrorKind::Config, std::string(to_string(kind_)) + ": " + f.error().what);
  if (error_ == ReferenceErrorKind::Ci) return Value{f->value, t95(n, params) * f->error};
  if (error_ == ReferenceErrorKind::MonteCarlo) return monte_carlo(f->value);
  return Value{f->value, f->error};
}

Result<Value> ReferenceModel::at(double t) const {
  if (constant_) return *constant_;
  const auto& p = included_;
  if (!is_interpolation(kind_)) return regression_at(t);
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
  return fail(ErrorKind::Config, "unsupported fit");
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
                  "The unknowns' stored values beside the predicted ones"),
          boolean("skip_reviewed", "Keep reviewed values", "Fit", false,
                  "Values already marked reviewed are shown but not refitted or saved")};
}

SchemaPtr make_blank_schema() {
  auto row = make_schema("figure.blank_fit.isotope", "Isotope",
                         {text("isotope", "Isotope", "Isotope", "Ar40"),
                          choice("fit", "Fit", "Isotope", fit_choices(), "preceding"),
                          choice("error", "Error", "Isotope", {"SEM", "SD", "MSEM", "CI", "MC"}, "SEM")});
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
                          choice("error", "Error", "Ratio", {"SEM", "SD", "MSEM", "CI", "MC"}, "SEM"),
                          choice("mode", "Mode", "Ratio", {"ic_factor", "source_correction"}, "ic_factor")});
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
  bool source_correction = false;
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
      s.source_correction = row.get_string("mode") == "source_correction";
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
  const bool skip_reviewed = o.get_bool("skip_reviewed");
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
    int reviewed_kept = 0;
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
      // The rows to store: blanks per isotope key; IC factors for the
      // denominator, or per Ar36..Ar39 detector from a source correction.
      struct Out {
        std::string key;
        Value value;
        bool reviewed = false;
      };
      std::vector<Out> outs;
      if (blanks) {
        for (const auto& iso : a.isotopes)
          if (iso.isotope == spec.isotope || iso.key == spec.isotope) outs.push_back({iso.key, *v, iso.blank_reviewed});
      } else if (!spec.source_correction) {
        outs.push_back({spec.denominator, *v, isotope_on(a, spec.denominator)->ic_reviewed});
      } else if (v->value > 0.0) {
        for (const auto& [name, mass] : kSourceMasses) {
          const IsotopeData* iso = a.find_by_isotope(name);
          if (!iso || iso->detector.empty() ||
              std::any_of(outs.begin(), outs.end(), [&](const Out& o) { return o.key == iso->detector; }))
            continue;
          const double k = std::log(kMass40 / mass) / std::log(kMass40 / kMass36);
          outs.push_back({iso->detector,
                          Value{std::pow(v->value, k), std::abs(k) * std::pow(v->value, k - 1.0) * v->error},
                          iso->ic_reviewed});
        }
      }
      for (const auto& out : outs) {
        if (skip_reviewed && out.reviewed) {
          ++reviewed_kept;
          continue;
        }
        auto& entry = by_uuid[a.uuid];
        entry.uuid = a.uuid;
        entry.runid = a.runid;
        entry.heads = a.heads;
        ReferenceRowFit row;
        row.key = out.key;
        row.value = out.value;
        row.fit = spec.fit;
        row.error = spec.error;
        if (!blanks) {
          row.reference_detector = spec.numerator;
          row.standard_ratio = spec.standard_ratio;
          row.source_correction = spec.source_correction;
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
        std::snprintf(buf, sizeof buf, "MSWD %.3g%s", *model->mswd(), model->weighted() ? "  weighted" : "");
        t.lines.push_back(buf);
      }
      if (spec.source_correction) t.lines.push_back("source correction (Ar36..Ar39)");
      if (reviewed_kept > 0) t.lines.push_back(std::to_string(reviewed_kept) + " reviewed value(s) kept");
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
