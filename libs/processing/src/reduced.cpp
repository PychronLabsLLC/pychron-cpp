#include "pychron/processing/reduced.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

constexpr std::pair<Stage, std::string_view> kStageNames[] = {
    {Stage::Intercept, "intercept"},
    {Stage::Baseline, "baseline"},
    {Stage::Blank, "blank"},
    {Stage::IcFactor, "ic_factor"},
    {Stage::BaselineCorrected, "bs_corrected"},
    {Stage::BlankCorrected, "bk_corrected"},
    {Stage::IcCorrected, "ic_corrected"},
    {Stage::DecayCorrected, "decay_corrected"},
    {Stage::InterferenceCorrected, "interference_corrected"},
};

// An unknown value enters as an exact NaN: UFloat::variable needs a finite
// sigma, and every stage built on it is then NaN rather than a number.
r::Measured measured(const Value& v) {
  if (!v.known()) return {std::numeric_limits<double>::quiet_NaN(), 0.0};
  return {v.value, v.error};
}

std::optional<r::ArgonIsotope> argon(std::string_view name) {
  for (const auto iso : r::kArgonKeys)
    if (r::to_string(iso) == name) return iso;
  return std::nullopt;
}

// "Ar40 intercept value" / "... error" for each number of `v` that is unknown.
void add_unknown(std::string& out, const IsotopeData& iso, std::string_view what, const Value& v) {
  for (const auto& [part, x] : {std::pair<std::string_view, double>{"value", v.value}, {"error", v.error}}) {
    if (std::isfinite(x)) continue;
    if (!out.empty()) out += ", ";
    out.append(iso.key).append(" ").append(what).append(" ").append(part);
  }
}

}  // namespace

std::string_view to_string(Stage stage) noexcept {
  for (const auto& [s, n] : kStageNames)
    if (s == stage) return n;
  return "intercept";
}

std::optional<Stage> parse_stage(std::string_view text) noexcept {
  for (const auto& [s, n] : kStageNames)
    if (n == text) return s;
  if (text == "bs") return Stage::Baseline;
  if (text == "bk") return Stage::Blank;
  if (text == "ic") return Stage::IcFactor;
  return std::nullopt;
}

const IsotopeStages* ReducedAnalysis::find(std::string_view key) const {
  for (const auto& iso : isotopes)
    if (iso.key == key) return &iso;
  return nullptr;
}

std::optional<r::UFloat> ReducedAnalysis::stage(std::string_view key, Stage stage) const {
  const IsotopeStages* found = find(key);
  if (!found && analysis) {
    if (const auto* data = analysis->find_by_isotope(key)) found = find(data->key);
  }
  if (!found) return std::nullopt;
  auto it = found->values.find(stage);
  if (it == found->values.end()) return std::nullopt;
  return it->second;
}

ReducedPtr reduce_analysis(AnalysisPtr analysis, const ReductionSettings& settings) {
  auto out = std::make_shared<ReducedAnalysis>();
  out->analysis = analysis;
  if (!analysis) return out;
  const Analysis& a = *analysis;
  const bool blank_corrected = r::corrects_for_blank(a.analysis_type);

  // One signal per isotope; the argon five feed reduce() with these same
  // variables so every stage correlates with the reduced values.
  std::array<std::optional<r::IsotopeSignal>, 5> argon_signals;
  std::array<const IsotopeData*, 5> argon_data{};
  for (const auto& iso : a.isotopes) {
    r::MeasuredSignal m;
    m.intercept = measured(iso.intercept);
    m.baseline = measured(iso.baseline);
    m.blank = measured(iso.blank);
    m.ic_factor = measured(iso.ic_factor);
    m.include_baseline_error = iso.include_baseline_error;
    m.correct_for_blank = blank_corrected;
    const auto ar = argon(iso.isotope);
    r::IsotopeSignal s = r::make_signal(ar.value_or(r::ArgonIsotope::Ar40), m);

    IsotopeStages st;
    st.key = iso.key;
    st.values[Stage::Intercept] = s.intercept;
    st.values[Stage::Baseline] = s.baseline;
    st.values[Stage::Blank] = s.blank;
    st.values[Stage::IcFactor] = s.ic_factor;
    st.values[Stage::BaselineCorrected] = r::baseline_corrected(s);
    st.values[Stage::BlankCorrected] = r::non_detector_corrected(s);
    st.values[Stage::IcCorrected] = r::corrected_intensity(s);
    st.values[Stage::DecayCorrected] = st.values[Stage::IcCorrected];
    out->isotopes.push_back(std::move(st));

    // The exact-key isotope wins ("Ar40" over "H1:Ar40").
    if (ar && (!argon_signals[r::index(*ar)] || iso.key == iso.isotope)) {
      argon_signals[r::index(*ar)] = s;
      argon_data[r::index(*ar)] = &iso;
    }
  }

  const bool any_argon = std::any_of(argon_signals.begin(), argon_signals.end(), [](const auto& s) { return s.has_value(); });
  if (!any_argon) return out;

  r::ReductionInput in;
  for (const auto iso : r::kArgonKeys) {
    if (argon_signals[r::index(iso)]) {
      in.isotopes[r::index(iso)] = *argon_signals[r::index(iso)];
    } else {
      r::MeasuredSignal zero;
      zero.correct_for_blank = blank_corrected;
      in.isotopes[r::index(iso)] = r::make_signal(iso, zero);
    }
  }
  in.constants = a.context.constants.value_or(r::constants_preset(settings.preset));
  in.constants.include_decay_error = settings.include_decay_error || in.constants.include_decay_error;
  in.constants.use_irradiation_endtime = settings.use_irradiation_endtime || in.constants.use_irradiation_endtime;
  in.production = r::make_production_variables(a.context.production.value_or(r::ProductionRatios{}));
  if (!a.context.chronology.empty())
    in.irradiation = r::irradiation_from_doses(a.context.chronology, static_cast<std::int64_t>(std::llround(a.timestamp)),
                                               in.constants.use_irradiation_endtime);
  if (a.context.flux) {
    in.j = r::make_j(*a.context.flux);
    in.position_jerr = a.context.flux->position_jerr;
    in.lambda_k_total = a.context.flux->lambda_k_total;
  }
  in.fixed_k3739 = a.context.fixed_k3739;

  out->constants = in.constants;
  out->j = in.j;
  out->lambda_k_total = in.lambda_k_total;
  // A stored number the source does not have is not a zero signal: no result
  // rather than an age from it.
  std::string unknown;
  for (const IsotopeData* iso : argon_data) {
    if (!iso) continue;
    add_unknown(unknown, *iso, "intercept", iso->intercept);
    add_unknown(unknown, *iso, "baseline", iso->baseline);
    add_unknown(unknown, *iso, "blank", iso->blank);
    add_unknown(unknown, *iso, "IC factor", iso->ic_factor);
  }
  if (!unknown.empty()) {
    out->reduction_error = "not reducible: no stored " + unknown;
    return out;
  }
  auto result = r::reduce(in);
  if (!result) {
    out->reduction_error = result.error().what;
    return out;
  }
  out->arar = std::move(*result);
  // Decay and interference stages for the isotopes that fed reduce().
  for (const auto iso : r::kArgonKeys) {
    if (!argon_signals[r::index(iso)]) continue;
    const auto* data = a.find_by_isotope(r::to_string(iso));
    if (!data) continue;
    for (auto& st : out->isotopes) {
      if (st.key != data->key) continue;
      st.values[Stage::DecayCorrected] = out->arar->corrected[r::index(iso)];
      st.values[Stage::InterferenceCorrected] = out->arar->f.interference_corrected[r::index(iso)];
    }
  }
  return out;
}

}  // namespace pychron::processing
