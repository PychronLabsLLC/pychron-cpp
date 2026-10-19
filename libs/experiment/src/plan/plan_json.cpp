// MeasurementPlan -> JSON golden form (see plan_json.hpp).

#include "pychron/experiment/plan/plan_json.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace pychron::experiment::plan {
namespace {

using nlohmann::json;

std::string str(std::string_view s) { return std::string(s); }

std::string_view error_name(reduction::ErrorType e) {
  switch (e) {
    case reduction::ErrorType::Sem:
      return "sem";
    case reduction::ErrorType::Sd:
      return "sd";
  }
  return "unknown";
}

std::string_view time_zero_kind_name(TimeZeroKind k) {
  switch (k) {
    case TimeZeroKind::OnInletClose:
      return "on_inlet_close";
    case TimeZeroKind::OnFirstCount:
      return "on_first_count";
    case TimeZeroKind::Offset:
      return "offset";
    case TimeZeroKind::Now:
      return "now";
  }
  return "unknown";
}

std::string_view position_kind_name(PositionKind k) { return k == PositionKind::Baseline ? "baseline" : "signal"; }

// An unset optional is null; a set one goes through `f`.
template <typename T, typename F>
json opt(const std::optional<T>& v, F&& f) {
  return v ? json(std::forward<F>(f)(*v)) : json(nullptr);
}

template <typename T>
json opt(const std::optional<T>& v) {
  return opt(v, [](const T& x) { return x; });
}

json fit_map(const std::map<std::string, reduction::FitKind>& m) {
  json out = json::object();
  for (const auto& [key, kind] : m) out[key] = str(reduction::to_string(kind));
  return out;
}

json outliers_json(const reduction::OutlierSpec& o) {
  return json{{"enabled", o.enabled}, {"iterations", o.iterations}, {"std_devs", o.std_devs}};
}

json fits_json(const Fits& f) {
  return json{{"signal", fit_map(f.signal)},
              {"baseline", fit_map(f.baseline)},
              {"error", str(error_name(f.error))},
              {"outliers", outliers_json(f.outliers)}};
}

json time_zero_json(const TimeZero& t) {
  return json{{"kind", str(time_zero_kind_name(t.kind))}, {"offset_s", t.offset_s}};
}

json equilibration_json(const Equilibration& e) {
  return json{{"inlet", e.inlet},
              {"outlet", e.outlet},
              {"time_s", e.time_s},
              {"inlet_delay_s", e.inlet_delay_s},
              {"close_inlet", e.close_inlet}};
}

json truncation_json(const Truncation& t) { return json{{"check", t.check}, {"start", t.start}}; }

json whiff_check_json(const WhiffCheck& c) { return json{{"check", c.check}, {"action", c.action}}; }

// The keys of the plan file's `at` table: which one is present says which target.
json at_json(const At& at) {
  return std::visit(
      [](const auto& a) -> json {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, AtLabel>) return json{{"label", a.label}, {"detector", a.detector}};
        else if constexpr (std::is_same_v<A, AtMass>) return json{{"mass", a.mass}, {"detector", a.detector}};
        else if constexpr (std::is_same_v<A, AtDac>) return json{{"dac", a.dac}};
        else return json{{"hv", a.hv}, {"detector", a.detector}};
      },
      at);
}

json collect_json(const CollectEntry& e) {
  return json{{"label", e.label},
              {"detector", e.detector},
              {"active", e.active},
              {"deflection", opt(e.deflection)},
              {"protect", e.protect}};
}

json position_json(const Position& p) {
  json collect = json::array();
  for (const auto& e : p.collect) collect.push_back(collect_json(e));
  return json{{"collect", std::move(collect)},
              {"at", opt(p.at, at_json)},
              {"kind", str(position_kind_name(p.kind))},
              {"counts", p.counts},
              {"settle_s", p.settle_s},
              {"integration_s", opt(p.integration_s)},
              {"optics", p.optics},
              {"table", p.table}};
}

json fit_override_json(const FitOverride& f) {
  return json{{"fit", opt(f.fit, [](reduction::FitKind k) { return str(reduction::to_string(k)); })},
              {"error", opt(f.error, [](reduction::ErrorType e) { return str(error_name(e)); })},
              {"outliers", opt(f.outliers, outliers_json)}};
}

json peak_center_json(const PeakCenterStep& s) {
  return json{{"label", s.label},
              {"detectors", s.detectors},
              {"extra_detectors", s.extra_detectors},
              {"config", s.config},
              {"write_table", s.write_table},
              {"min_signal", s.min_signal}};
}

json set_json(const SetStep& s) {
  return json{{"deflection", s.deflection}, {"gain", s.gain},
              {"source", s.source},         {"hv", opt(s.hv)},
              {"cdd_voltage", opt(s.cdd_voltage)}, {"integration_s", opt(s.integration_s)}};
}

json park_json(const ParkStep& s) {
  return json{{"label", s.label}, {"mass", opt(s.mass)}, {"detector", s.detector}};
}

json segment_json(const Segment& s) {
  json positions = json::array();
  for (const auto& p : s.positions) positions.push_back(position_json(p));
  json fits = json::object();
  for (const auto& [key, f] : s.fits) fits[key] = fit_override_json(f);
  json truncations = json::array();
  for (const auto& t : s.truncations) truncations.push_back(truncation_json(t));
  json checks = json::array();
  for (const auto& c : s.checks) checks.push_back(whiff_check_json(c));
  return json{{"kind", str(to_string(s.kind))},
              {"when", str(to_string(s.when))},
              {"cycles", s.cycles},
              {"integration_s", s.integration_s},
              {"time_zero", opt(s.time_zero, time_zero_json)},
              {"table", s.table},
              {"positions", std::move(positions)},
              {"fits", std::move(fits)},
              {"truncations", std::move(truncations)},
              {"checks", std::move(checks)},
              {"peak_center", opt(s.peak_center, peak_center_json)},
              {"set", opt(s.set, set_json)},
              {"blank_beam", opt(s.blank_beam)},
              {"park", opt(s.park, park_json)},
              {"delay_s", opt(s.delay_s)}};
}

json gas_json(const Gas& g) {
  json segments = json::array();
  for (const auto& s : g.segments) segments.push_back(segment_json(s));
  json out{{"name", g.name},
           {"equilibration", equilibration_json(g.equilibration)},
           {"source_dark_phase", g.source_dark_phase},
           {"time_zero", opt(g.time_zero, time_zero_json)},
           {"fits", opt(g.fits, fits_json)},
           {"segments", std::move(segments)},
           {"implicit", g.implicit}};
  // Only when there: a gas without keeps the golden it had (spec 11).
  if (g.prepare) {
    json vars = json::object();
    for (const auto& [k, v] : g.prepare->variables) std::visit([&, &key = k](const auto& x) { vars[key] = x; }, v);
    out["prepare"] = {{"script", g.prepare->script}, {"variables", std::move(vars)}};
  }
  if (g.pump_out) out["pump_out"] = {{"valve", g.pump_out->valve}, {"time_s", g.pump_out->time_s}};
  return out;
}

}  // namespace

std::string to_json(const MeasurementPlan& plan) {
  json gases = json::array();
  for (const auto& g : plan.gases) gases.push_back(gas_json(g));
  json truncations = json::array();
  for (const auto& t : plan.conditionals.truncations) truncations.push_back(truncation_json(t));
  json expose = json::array();
  for (const auto& e : plan.expose) expose.push_back(json{{"path", e.path}, {"label", e.label}});

  // nlohmann::json's object is a std::map: keys come out sorted.
  json out{
      {"info",
       {{"name", plan.info.name},
        {"instrument_family", plan.info.instrument_family},
        {"description", plan.info.description},
        {"analysis_types", plan.info.analysis_types}}},
      {"detectors", {{"reference", plan.detectors.reference}, {"exclude", plan.detectors.exclude}}},
      {"gases", std::move(gases)},
      {"fits", fits_json(plan.fits)},
      {"conditionals", {{"include", plan.conditionals.include}, {"truncations", std::move(truncations)}}},
      {"expose", std::move(expose)},
      {"hook", opt(plan.hook)},
  };
  // Only when there are any: a plan without keeps the golden it had.
  if (!plan.baseline_modifiers.empty()) {
    json mods = json::object();
    for (const auto& [det, m] : plan.baseline_modifiers)
      mods[det] = {{"variable", m.variable}, {"model", m.model}, {"error", m.error}};
    out["baseline_modifiers"] = std::move(mods);
  }
  return out.dump(2) + "\n";
}

}  // namespace pychron::experiment::plan
