#include "pychron/processing/reduced.hpp"

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
};

// An unknown value enters as an exact NaN: UFloat::variable needs a finite
// sigma, and every stage built on it is then NaN rather than a number.
r::Measured measured(const Value& v) {
  if (!v.known()) return {std::numeric_limits<double>::quiet_NaN(), 0.0};
  return {v.value, v.error};
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

ReducedPtr reduce_analysis(AnalysisPtr analysis) {
  auto out = std::make_shared<ReducedAnalysis>();
  out->analysis = std::move(analysis);
  if (!out->analysis) return out;
  const Analysis& a = *out->analysis;
  const bool blank_corrected = r::corrects_for_blank(a.analysis_type);

  // One signal per isotope, kept: a domain's reduction builds on these same
  // variables, so its values correlate with every stage here.
  out->isotope_signals.reserve(a.isotopes.size());
  out->isotopes.reserve(a.isotopes.size());
  for (const auto& iso : a.isotopes) {
    r::MeasuredSignal m;
    m.intercept = measured(iso.intercept);
    m.baseline = measured(iso.baseline);
    m.blank = measured(iso.blank);
    m.ic_factor = measured(iso.ic_factor);
    m.include_baseline_error = iso.include_baseline_error;
    m.correct_for_blank = blank_corrected;
    r::IsotopeSignal s = r::make_signal(iso.isotope, m);

    IsotopeStages st;
    st.key = iso.key;
    st.values[Stage::Intercept] = s.intercept;
    st.values[Stage::Baseline] = s.baseline;
    st.values[Stage::Blank] = s.blank;
    st.values[Stage::IcFactor] = s.ic_factor;
    st.values[Stage::BaselineCorrected] = r::baseline_corrected(s);
    st.values[Stage::BlankCorrected] = r::non_detector_corrected(s);
    st.values[Stage::IcCorrected] = r::corrected_intensity(s);
    out->isotopes.push_back(std::move(st));
    out->isotope_signals.push_back(std::move(s));
  }
  return out;
}

}  // namespace pychron::processing
