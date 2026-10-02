#include "pychron/systems/jobs/peak_center.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>

#include <toml++/toml.hpp>

namespace pychron::jobs {

namespace {

Unexpected<Error> no_peak(const std::string& what) { return fail(ErrorKind::Config, what, "peak_center"); }

std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.10g", v);
  return b;
}

// pychron calculate_peak_center on x-sorted points.
Result<PeakShape> fit_sorted(const std::vector<double>& x, const std::vector<double>& y, double percent,
                             double min_peak_height, bool test_peak_flat, bool ignore_max) {
  const std::size_t n = x.size();
  if (n == 0) return no_peak("PeakCenterError: no points");
  const auto max_it = std::max_element(y.begin(), y.end());  // first maximum, as numpy argmax
  const double ma = *max_it;
  const std::size_t max_i = static_cast<std::size_t>(max_it - y.begin());
  if (!ignore_max && ma < min_peak_height)
    return no_peak("No peak greater than " + num(min_peak_height) + ". max = " + num(ma));
  if (max_i <= 3 || max_i + 3 >= n)
    return no_peak("PeakCenterError: peak not well centered. Max intensity too close to scan limits");

  const double threshold = ma * (1 - percent / 100.0);
  // Low side: walk down from the max; without a crossing pychron's loop ends on index 1.
  std::size_t lo = 1;
  for (std::size_t i = max_i; i >= 1; --i) {
    lo = i;
    if (y[i] < threshold) break;
  }
  // High side: without a crossing the loop ends on the last index.
  std::size_t hi = n - 1;
  for (std::size_t i = max_i; i < n; ++i) {
    hi = i;
    if (y[i] < threshold) break;
  }
  const double lx = x[lo], hx = x[hi];
  if (hx - lx < 0)
    return no_peak("unable to find peak bounds high_pos < low_pos. " + num(hx) + " < " + num(lx));

  PeakShape s;
  s.low_x = lx;
  s.high_x = hx;
  s.low_y = y[lo];
  s.high_y = y[hi];
  s.center_x = (hx + lx) / 2.0;
  s.center_y = ma;
  s.max_x = x[max_i];
  s.max_y = ma;

  if (test_peak_flat) {
    std::size_t ccx = 0;
    for (std::size_t i = 1; i < n; ++i)
      if (std::fabs(x[i] - s.center_x) < std::fabs(x[ccx] - s.center_x)) ccx = i;
    // y[ccx-2 : ccx+2]; clamped at the scan start (pychron would wrap).
    const std::size_t b = ccx >= 2 ? ccx - 2 : 0, e = std::min(n, ccx + 2);
    if (e - b >= 2) {
      const std::size_t m = e - b;
      double xm = 0, ym = 0;
      for (std::size_t k = 0; k < m; ++k) {
        xm += static_cast<double>(k);
        ym += y[b + k];
      }
      xm /= static_cast<double>(m);
      ym /= static_cast<double>(m);
      double sxy = 0, sxx = 0, ss = 0;
      for (std::size_t k = 0; k < m; ++k) {
        const double dx = static_cast<double>(k) - xm, dy = y[b + k] - ym;
        sxy += dx * dy;
        sxx += dx * dx;
        ss += dy * dy;
      }
      const double slope = sxy / sxx;
      const double std = std::sqrt(ss / static_cast<double>(m));  // numpy std: population
      if (std > 5 && std::fabs(slope) < 1)
        return no_peak("No peak plateau std = " + num(std) + " (>5) slope = " + num(slope) + " (<1)");
    }
  }
  return s;
}

void sort_points(std::vector<std::pair<double, double>>& points, std::vector<double>& x, std::vector<double>& y) {
  std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  x.clear();
  y.clear();
  for (const auto& [px, py] : points) {
    x.push_back(px);
    y.push_back(py);
  }
}

std::optional<double> finite(double v) {
  if (!std::isfinite(v)) return std::nullopt;
  return v;
}

}  // namespace

std::optional<double> peak_resolution(const std::vector<std::pair<double, double>>& points_in) {
  auto points = points_in;
  std::vector<double> x, y;
  sort_points(points, x, y);
  auto s = fit_sorted(x, y, 95, 1.0, true, true);
  if (!s) return std::nullopt;
  return finite(s->center_x / (s->high_x - s->low_x));
}

std::pair<std::optional<double>, std::optional<double>> peak_resolving_power(
    const std::vector<std::pair<double, double>>& points_in) {
  auto points = points_in;
  std::vector<double> x, y;
  sort_points(points, x, y);
  // pychron names the 95% fit "5" and the 5% fit "95"; kept so the numbers match.
  auto p5 = fit_sorted(x, y, 95, 1.0, false, true);
  auto p95 = fit_sorted(x, y, 5, 1.0, false, true);
  if (!p5 || !p95) return {std::nullopt, std::nullopt};
  const double ldelta = std::fabs(p95->low_x - p5->low_x);
  const double hdelta = std::fabs(p95->high_x - p5->high_x);
  return {finite((p5->low_x + ldelta / 2) / ldelta), finite((p95->high_x + hdelta / 2) / hdelta)};
}

Result<PeakShape> find_peak_center(std::vector<std::pair<double, double>> points, const PeakFitOptions& o) {
  for (const auto& [px, py] : points)
    if (!std::isfinite(px) || !std::isfinite(py)) return no_peak("PeakCenterError: non-finite point");
  std::vector<double> x, y;
  sort_points(points, x, y);
  auto s = fit_sorted(x, y, o.percent, o.min_peak_height, o.test_peak_flat, o.ignore_max);
  if (!s) return s;
  s->resolution = peak_resolution(points);
  std::tie(s->resolving_power_low, s->resolving_power_high) = peak_resolving_power(points);
  return s;
}

// ---- config ---------------------------------------------------------------------

namespace {

Result<void> validate(const PeakCenterConfig& c) {
  auto bad = [&](const std::string& m) { return fail(ErrorKind::Config, "peak center '" + c.name + "': " + m); };
  if (!(c.window > 0)) return bad("window must be positive");
  if (!(c.step > 0) || c.step >= c.window) return bad("step must be positive and smaller than window");
  if (!(c.percent > 0 && c.percent < 100)) return bad("percent must be in (0, 100)");
  if (c.n_tries < 1) return bad("n_tries must be >= 1");
  if (c.peak_shift_threshold < 0) return bad("peak_shift_threshold must be >= 0");
  if (c.isotope.empty() && !c.center) return bad("needs an isotope (or a center)");
  return {};
}

Duration seconds(double s) {
  return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(s));
}

}  // namespace

Result<std::map<std::string, PeakCenterConfig>> parse_peak_center_configs(std::string_view text,
                                                                         std::string_view file) {
  auto parsed = toml::parse(text, file);
  if (!parsed) return fail(ErrorKind::Config, std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();
  static const std::set<std::string_view> known{
      "detector", "isotope",  "additional_detectors", "center",        "window",
      "step",     "percent",  "min_peak_height",      "test_peak_flat", "n_tries",
      "direction", "integration_s", "settle_s",       "baseline_timeout_s", "dac_offset",
      "peak_shift_threshold", "update_table", "propagate"};
  std::map<std::string, PeakCenterConfig> out;
  for (const auto& [name_key, node] : root) {
    const std::string name(name_key.str());
    const std::string where = std::string(file) + ": [" + name + "]";
    const auto* t = node.as_table();
    if (t == nullptr) return fail(ErrorKind::Config, where + ": expected a table");
    for (const auto& [k, v] : *t)
      if (!known.contains(k.str())) return fail(ErrorKind::Config, where + ": unknown key '" + std::string(k.str()) + "'");
    PeakCenterConfig c;
    c.name = name;
    std::string error;
    auto num_key = [&](const char* key, double& out_v) {
      if (const auto* n = t->get(key)) {
        if (auto v = n->value<double>()) out_v = *v;
        else error = std::string(key) + " must be a number";
      }
    };
    auto bool_key = [&](const char* key, bool& out_v) {
      if (const auto* n = t->get(key)) {
        if (n->is_boolean()) out_v = *n->value<bool>();
        else error = std::string(key) + " must be a boolean";
      }
    };
    auto str_key = [&](const char* key, std::string& out_v) {
      if (const auto* n = t->get(key)) {
        if (n->is_string()) out_v = *n->value<std::string>();
        else error = std::string(key) + " must be a string";
      }
    };
    str_key("detector", c.detector);
    str_key("isotope", c.isotope);
    if (const auto* n = t->get("additional_detectors")) {
      const auto* arr = n->as_array();
      if (arr == nullptr) error = "additional_detectors must be an array of names";
      else
        for (const auto& e : *arr) {
          if (auto s = e.value<std::string>()) c.additional_detectors.push_back(*s);
          else error = "additional_detectors must be an array of names";
        }
    }
    if (t->contains("center")) {
      double v = 0;
      num_key("center", v);
      c.center = v;
    }
    num_key("window", c.window);
    num_key("step", c.step);
    num_key("percent", c.percent);
    num_key("min_peak_height", c.min_peak_height);
    bool_key("test_peak_flat", c.test_peak_flat);
    if (const auto* n = t->get("n_tries")) {
      if (n->is_integer()) c.n_tries = static_cast<int>(*n->value<std::int64_t>());
      else error = "n_tries must be an integer";
    }
    std::string direction = "increase";
    str_key("direction", direction);
    if (direction != "increase" && direction != "decrease") error = "direction must be increase or decrease";
    c.increase = direction == "increase";
    double integ = 0, settle = 0, timeout = 10;
    num_key("integration_s", integ);
    num_key("settle_s", settle);
    num_key("baseline_timeout_s", timeout);
    if (integ < 0 || settle < 0 || timeout < 0) error = "times must be >= 0";
    c.integration = seconds(integ);
    c.settle = seconds(settle);
    c.baseline_timeout = seconds(timeout);
    num_key("dac_offset", c.dac_offset);
    num_key("peak_shift_threshold", c.peak_shift_threshold);
    bool_key("update_table", c.update_table);
    bool_key("propagate", c.propagate);
    if (!error.empty()) return fail(ErrorKind::Config, where + ": " + error);
    if (auto v = validate(c); !v) return fail(ErrorKind::Config, where + ": " + v.error().what);
    out.emplace(name, std::move(c));
  }
  return out;
}

// ---- runner ---------------------------------------------------------------------

namespace {

class Runner {
 public:
  Runner(Spectrometer& spec, const PeakCenterConfig& cfg, Progress& progress, CancelToken& cancel,
         const PeakCenterOptions& options)
      : spec_(spec), cfg_(cfg), progress_(progress), cancel_(cancel), options_(options) {}

  Result<PeakCenterResult> run() {
    if (auto v = validate(cfg_); !v) return fail(v.error());
    r_.config = cfg_;
    r_.isotope = cfg_.isotope;
    r_.detector = cfg_.detector.empty() ? spec_.reference_detector() : cfg_.detector;
    for (const auto& d : with_additional())
      if (!spec_.detectors().find(d)) return fail(ErrorKind::Config, "unknown detector '" + d + "'", "peak_center");
    if (cfg_.center) {
      r_.initial_center = *cfg_.center;
    } else {
      auto mass = spec_.mass_of(spectrometer::PositionTarget{spectrometer::Isotope{cfg_.isotope}, r_.detector});
      if (!mass) return fail(mass.error());
      auto native = spec_.native_for(*mass, r_.detector);
      if (!native) return fail(native.error());
      r_.initial_center = *native;
    }

    std::optional<double> center;
    bool strong = false;
    for (int i = 0; i < cfg_.n_tries; ++i) {
      if (cancel_.cancelled()) return fail(ErrorKind::Cancelled, "peak center cancelled", "peak_center");
      const double c = center.value_or(r_.initial_center);
      const double d = strong ? cfg_.window : center ? cfg_.window * (0.1 * i + 1) : cfg_.window * (i + 1);
      const double start = cfg_.increase ? c - d : c + d;
      const double end = cfg_.increase ? c + d : c - d;
      progress_.report({0, 0, "peak center try " + std::to_string(i + 1) + ": " + num(std::min(start, end)) +
                                  " - " + num(std::max(start, end)), std::nullopt});
      auto t = attempt(start, end);
      if (!t) return fail(t.error());
      auto outcome = conclude();
      if (outcome == Outcome::Done) break;
      center = outcome == Outcome::Retry ? retry_center_ : std::nullopt;
      strong = retry_strong_;
    }
    if (!r_.ok && r_.message.empty()) r_.message = "centering failed";
    if (options_.bus != nullptr) options_.bus->publish(PeakCenterDone{r_});
    return r_;
  }

 private:
  enum class Outcome { Done, Retry, RetryFromStart };

  std::vector<DetectorId> with_additional() const {
    std::vector<DetectorId> out{r_.detector};
    for (const auto& d : cfg_.additional_detectors)
      if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    return out;
  }

  Duration integration() const {
    return cfg_.integration > Duration::zero() ? cfg_.integration : spec_.acquisition().integration();
  }

  void sleep(Duration d) const {
    if (d <= Duration::zero()) return;
    if (options_.sweep.sleep) options_.sweep.sleep(d);
    else spec_.sleep(d);  // the spectrometer's clock, simulated or not
  }

  Result<std::optional<spectrometer::Reading>> read_one() {
    auto readings = spec_.acquire(std::size_t{1});
    if (!readings) return fail(readings.error());
    if (readings->empty()) return std::optional<spectrometer::Reading>{};
    return std::optional<spectrometer::Reading>(readings->front());
  }

  double value_of(const spectrometer::Reading& r) const {
    auto it = r.values.find(r_.detector);
    return it != r.values.end() && it->second ? it->second->mean : 0.0;
  }

  Result<void> attempt(double start, double end) {
    PeakCenterTry t;
    t.start = start;
    t.end = end;
    // Reference intensity on peak, then wait at the start for the baseline.
    if (auto m = spec_.move_native((start + end) / 2); !m) return fail(m.error());
    sleep(2 * integration());
    auto on_peak = read_one();
    if (!on_peak) return fail(on_peak.error());
    const double current = *on_peak ? value_of(**on_peak) : 0.0;
    if (auto m = spec_.move_native(start); !m) return fail(m.error());
    sleep(2 * integration());
    const double tol = current * (1 - cfg_.percent / 100.0) / 2.0;
    std::optional<TimePoint> first;
    while (true) {
      if (cancel_.cancelled()) return fail(ErrorKind::Cancelled, "peak center cancelled", "peak_center");
      auto r = read_one();
      if (!r) return fail(r.error());
      if (!*r) break;
      if (value_of(**r) <= tol) {
        t.baseline_reached = true;
        break;
      }
      if (!first) first = (*r)->ts;
      if ((*r)->ts - *first >= cfg_.baseline_timeout) break;  // proceed anyway, as pychron does
    }

    SweepSpec sweep;
    sweep.axis = SweepAxis::magnet();
    sweep.start = start;
    sweep.stop = end;
    sweep.step = cfg_.step;
    sweep.integration = cfg_.integration;
    sweep.settle = cfg_.settle;
    sweep.record = with_additional();
    Sweep runner(options_.sweep);
    auto points = runner.run(spec_, sweep, progress_, cancel_);
    t.points = runner.points();
    r_.tries.push_back(std::move(t));
    if (!points) return fail(points.error());
    return {};
  }

  std::vector<std::pair<double, double>> series(const DetectorId& det) const {
    std::vector<std::pair<double, double>> out;
    for (const auto& p : r_.tries.back().points)
      if (auto it = p.y.find(det); it != p.y.end()) out.emplace_back(p.x, it->second);
    return out;
  }

  Outcome conclude() {
    const PeakFitOptions fit{cfg_.percent, cfg_.min_peak_height, cfg_.test_peak_flat, false};
    const auto pts = series(r_.detector);
    auto shape = find_peak_center(pts, fit);
    r_.additional.clear();
    for (const auto& d : cfg_.additional_detectors) {
      auto s = find_peak_center(series(d), fit);
      r_.additional.push_back(DetectorPeak{d, s ? std::optional(*s) : std::nullopt, s ? "" : s.error().what});
    }
    retry_center_.reset();
    retry_strong_ = false;
    if (!shape) {
      r_.message = shape.error().what;
      if (pts.empty()) return Outcome::RetryFromStart;
      const auto top = std::max_element(pts.begin(), pts.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
      });
      retry_center_ = top->first;
      retry_strong_ = top->second > cfg_.min_peak_height * 5;
      return Outcome::Retry;
    }
    r_.shape = *shape;
    const double c = shape->center_x + cfg_.dac_offset;
    if (cfg_.peak_shift_threshold > 0 && std::fabs(c - r_.initial_center) > cfg_.peak_shift_threshold) {
      r_.message = "Peak center moved too much. current=" + num(c) + ", previous=" + num(r_.initial_center) +
                   ", dev=" + num(std::fabs(c - r_.initial_center)) + " > " + num(cfg_.peak_shift_threshold);
      r_.shape.reset();
      return Outcome::RetryFromStart;
    }
    r_.center = c;
    r_.ok = true;
    r_.message.clear();
    if (auto applied = apply(c); !applied) {
      r_.ok = false;
      r_.message = applied.error().what;
    }
    return Outcome::Done;
  }

  Result<void> apply(double c) {
    auto table_value = spec_.uncorrect(c, r_.detector);
    if (!table_value) return fail(table_value.error());
    r_.table_value = *table_value;
    if (cfg_.update_table && !cfg_.isotope.empty()) {
      if (auto u = spec_.update_table(r_.detector, cfg_.isotope, *table_value, cfg_.propagate); !u) return u;
      r_.table_updated = true;
      auto p = spec_.position(spectrometer::PositionTarget{spectrometer::Isotope{cfg_.isotope}, r_.detector});
      if (!p) return fail(p.error());
      return {};
    }
    auto m = spec_.move_native(c);
    if (!m) return fail(m.error());
    return {};
  }

  Spectrometer& spec_;
  const PeakCenterConfig& cfg_;
  Progress& progress_;
  CancelToken& cancel_;
  const PeakCenterOptions& options_;
  PeakCenterResult r_;
  std::optional<double> retry_center_;
  bool retry_strong_ = false;
};

}  // namespace

Result<PeakCenterResult> run_peak_center(Spectrometer& spectrometer, const PeakCenterConfig& config,
                                         Progress& progress, CancelToken& cancel, const PeakCenterOptions& options) {
  return Runner(spectrometer, config, progress, cancel, options).run();
}

JobSpec peak_center_job(PeakCenterConfig config, PeakCenterOptions options) {
  return JobSpec{"peak_center",
                 [config = std::move(config), options = std::move(options)](JobContext& ctx) -> Result<std::any> {
                   auto r = run_peak_center(ctx.spectrometer, config, ctx.progress, ctx.cancel, options);
                   if (!r) return fail(r.error());
                   return std::any(std::move(*r));
                 }};
}

}  // namespace pychron::jobs
