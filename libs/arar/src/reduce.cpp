#include "pychron/arar/reduce.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "pychron/arar/analysis_data.hpp"
#include "pychron/arar/quantities.hpp"

namespace pychron::arar {

namespace r = pychron::reduction;
namespace pp = pychron::processing;

namespace {

std::optional<r::ArgonIsotope> argon(std::string_view name) {
  for (const auto iso : r::kArgonKeys)
    if (r::to_string(iso) == name) return iso;
  return std::nullopt;
}

// "Ar40 intercept value" / "... error" for each of the two that is unknown.
void add_unknown(std::string& out, const std::string& what, double value, double error) {
  for (const auto& [part, x] : {std::pair<std::string_view, double>{"value", value}, {"error", error}}) {
    if (std::isfinite(x)) continue;
    if (!out.empty()) out += ", ";
    out.append(what).append(" ").append(part);
  }
}
void add_unknown(std::string& out, const pp::IsotopeData& iso, std::string_view what, const pp::Value& v) {
  add_unknown(out, iso.key + " " + std::string(what), v.value, v.error);
}

}  // namespace

std::optional<r::UFloat> ArArReduced::stage(std::string_view label, std::string_view stage) const {
  auto at = std::find(keys.begin(), keys.end(), label);
  if (at == keys.end()) {
    const auto by_name = std::find(isotopes.begin(), isotopes.end(), label);
    if (by_name == isotopes.end()) return std::nullopt;
    at = keys.begin() + (by_name - isotopes.begin());
  }
  const auto i = static_cast<std::size_t>(at - keys.begin());
  if (i >= isotope_stages.size()) return std::nullopt;
  const auto it = isotope_stages[i].find(stage);
  if (it == isotope_stages[i].end()) return std::nullopt;
  return it->second;
}

std::optional<r::UFloat> ArArReduced::named(std::string_view raw) const {
  const std::string_view name = canonical_name(raw);
  if (name == "age") return arar.ages ? std::optional<r::UFloat>(arar.ages->age) : std::nullopt;
  if (name == "age_w_j") return arar.ages ? std::optional<r::UFloat>(arar.ages->age_w_j_err) : std::nullopt;
  if (name == "age_w_position") return arar.ages ? std::optional<r::UFloat>(arar.ages->age_w_position_err) : std::nullopt;
  if (name == "F") return arar.f.f;
  if (name == "kca") return arar.kca;
  if (name == "cak") return arar.cak;
  if (name == "kcl") return arar.kcl;
  if (name == "clk") return arar.clk;
  if (name == "radiogenic_yield") return arar.f.radiogenic_yield;
  if (name == "rad40") return arar.f.rad40;
  if (name == "k39") return arar.f.interference_corrected[r::index(r::ArgonIsotope::Ar39)];
  // A J of its own each time, as the quantity always gave it: a ratio with an
  // age does not share the age's J variable (`j`, which the group ages use).
  if (name == "j") return j ? std::optional<r::UFloat>(r::UFloat::variable(j->nominal(), j->std_dev(), "J")) : std::nullopt;
  return std::nullopt;
}

std::vector<std::string> ArArReduced::names() const { return value_names(); }

const ArArReduced* reduced_of(const pp::ReducedAnalysis& reduced) noexcept {
  return dynamic_cast<const ArArReduced*>(reduced.domain.get());
}

Result<std::shared_ptr<const ArArReduced>> reduce_arar(const pp::ReducedAnalysis& reduced, const ArArSettings& settings) {
  if (!reduced.analysis) return fail(ErrorKind::Config, "no analysis to reduce");
  const pp::Analysis& a = *reduced.analysis;
  if (reduced.isotope_signals.size() != a.isotopes.size() || reduced.isotopes.size() != a.isotopes.size())
    return fail(ErrorKind::Config, "not reduced: run reduce before arar_reduce");
  const bool blank_corrected = r::corrects_for_blank(a.analysis_type);

  // The argon five feed reduce() with the signals core's reduce minted, so
  // every Ar-Ar value correlates with the core stages. The exact-key isotope
  // wins ("Ar40" over "H1:Ar40").
  std::array<std::optional<std::size_t>, 5> argon_at;
  for (std::size_t i = 0; i < a.isotopes.size(); ++i) {
    const auto& iso = a.isotopes[i];
    const auto ar = argon(iso.isotope);
    if (ar && (!argon_at[r::index(*ar)] || iso.key == iso.isotope)) argon_at[r::index(*ar)] = i;
  }
  const bool any_argon = std::any_of(argon_at.begin(), argon_at.end(), [](const auto& i) { return i.has_value(); });
  if (!any_argon) return fail(ErrorKind::Config, "no argon isotopes to reduce");
  // The flux, production and chronology are the Ar-Ar loader's: an analysis
  // from a source that was given none has nothing to reduce with.
  const ArArAnalysisData* references = analysis_data_of(a);
  if (references == nullptr) return fail(ErrorKind::Config, "no Ar-Ar reference data for this analysis");
  const ArArAnalysisData& inputs = *references;

  r::ReductionInput in;
  for (const auto iso : r::kArgonKeys) {
    if (const auto& at = argon_at[r::index(iso)]) {
      in.isotopes[r::index(iso)] = reduced.isotope_signals[*at];
    } else {
      r::MeasuredSignal zero;
      zero.correct_for_blank = blank_corrected;
      in.isotopes[r::index(iso)] = r::make_signal(iso, zero);
    }
  }
  in.constants = inputs.constants.value_or(r::constants_preset(settings.preset));
  in.constants.include_decay_error = settings.include_decay_error || in.constants.include_decay_error;
  in.constants.use_irradiation_endtime = settings.use_irradiation_endtime || in.constants.use_irradiation_endtime;
  in.production = r::make_production_variables(inputs.production.value_or(r::ProductionRatios{}));
  if (!inputs.chronology.empty())
    in.irradiation = r::irradiation_from_doses(inputs.chronology, static_cast<std::int64_t>(std::llround(a.timestamp)),
                                               in.constants.use_irradiation_endtime);
  in.fixed_k3739 = inputs.fixed_k3739;

  // A stored number the source does not have is not a zero signal or a zero
  // error: no result rather than an age from it.
  std::string unknown;
  for (const auto& at : argon_at) {
    if (!at) continue;
    const pp::IsotopeData& iso = a.isotopes[*at];
    add_unknown(unknown, iso, "intercept", iso.intercept);
    add_unknown(unknown, iso, "baseline", iso.baseline);
    add_unknown(unknown, iso, "blank", iso.blank);
    add_unknown(unknown, iso, "IC factor", iso.ic_factor);
  }
  if (const auto& flux = inputs.flux) {
    add_unknown(unknown, "J", flux->j.value, flux->j.error);
    if (flux->lambda_k_total) add_unknown(unknown, "lambda_k_total", flux->lambda_k_total->value, flux->lambda_k_total->error);
  }
  if (!unknown.empty()) return fail(ErrorKind::Config, "not reducible: no stored " + unknown);

  // An unknown was irradiated: without its production ratios every
  // interference correction is zero, without its chronology Ar37 and Ar39 are
  // not decay corrected. Its ratios stand, but it gets no J and so no age, and
  // reduction_error says why. Airs, blanks and cocktails need neither.
  std::string missing;
  if (a.analysis_type == "unknown") {
    if (!inputs.production) missing = "no production ratios";
    if (inputs.chronology.empty()) missing += (missing.empty() ? "" : ", ") + std::string("no chronology");
  }
  if (inputs.flux && missing.empty()) {
    in.j = r::make_j(*inputs.flux);
    in.position_jerr = inputs.flux->position_jerr;
    in.lambda_k_total = inputs.flux->lambda_k_total;
  }
  auto result = r::reduce(in);
  if (!result) return fail(result.error());

  auto out = std::make_shared<ArArReduced>();
  out->constants = in.constants;
  out->j = in.j;
  out->lambda_k_total = in.lambda_k_total;
  out->arar = std::move(*result);
  if (!missing.empty()) out->reduction_error = "no age: " + missing;

  // Decay and interference stages: the IC-corrected stage for an isotope
  // reduce() did not decay correct, reduce()'s own for the argon it was given.
  out->isotope_stages.resize(a.isotopes.size());
  out->keys.reserve(a.isotopes.size());
  out->isotopes.reserve(a.isotopes.size());
  for (std::size_t i = 0; i < a.isotopes.size(); ++i) {
    out->keys.push_back(a.isotopes[i].key);
    out->isotopes.push_back(a.isotopes[i].isotope);
    if (const auto ic = reduced.isotopes[i].values.find(pp::Stage::IcCorrected); ic != reduced.isotopes[i].values.end())
      out->isotope_stages[i].emplace("decay_corrected", ic->second);
  }
  for (const auto iso : r::kArgonKeys) {
    if (!argon_at[r::index(iso)]) continue;
    const auto* data = a.find_by_isotope(r::to_string(iso));
    if (!data) continue;
    for (std::size_t i = 0; i < a.isotopes.size(); ++i) {
      if (a.isotopes[i].key != data->key) continue;
      out->isotope_stages[i].insert_or_assign("decay_corrected", out->arar.corrected[r::index(iso)]);
      out->isotope_stages[i].insert_or_assign("interference_corrected", out->arar.f.interference_corrected[r::index(iso)]);
    }
  }
  return std::shared_ptr<const ArArReduced>(std::move(out));
}

pp::ReducedPtr attach_arar(const pp::ReducedAnalysis& reduced, const ArArSettings& settings) {
  auto out = std::make_shared<pp::ReducedAnalysis>(reduced);
  out->domain.reset();
  if (auto arar = reduce_arar(reduced, settings)) {
    out->reduction_error = (*arar)->reduction_error;
    out->domain = std::move(*arar);
  } else {
    out->reduction_error = arar.error().what;
  }
  return out;
}

pp::ReducedPtr reduce_with_arar(pp::AnalysisPtr analysis, const ArArSettings& settings) {
  pp::ReducedPtr core = pp::reduce_analysis(std::move(analysis));
  if (!core->analysis) return core;
  return attach_arar(*core, settings);
}

}  // namespace pychron::arar
