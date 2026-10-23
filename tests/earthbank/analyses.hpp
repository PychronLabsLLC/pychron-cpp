// Analyses for the mapping tests: a step-heated sample with a plateau and a
// sample of fusion analyses, built on the processing fixtures and then given
// what a real unknown has, so that every stage of the reduction differs from
// the one before it: blanks, a detector factor, an irradiation with power and
// time since, interference ratios, a J with an uncertainty.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "arar/arar_fixtures.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/arar/arar_types.hpp"

namespace pychron::earthbank::testing {

using AnalysisList = std::vector<processing::AnalysisPtr>;

// An irradiation of ten hours at full power, a hundred days before the run,
// with interference ratios of the usual size and a J known to 0.2%.
inline void irradiate_for_real(processing::Analysis& a) {
  arar::edit_analysis_data(a).flux = reduction::Flux{{0.0015, 3e-6}, 0.0, std::nullopt};
  reduction::ProductionRatios p;
  p.k4039 = {0.0085, 0.0002};
  p.k3839 = {0.0123, 0.0001};
  p.k3739 = {0.0002, 0.00002};
  p.ca3937 = {0.00068, 0.00002};
  p.ca3837 = {0.00002, 0.000002};
  p.ca3637 = {0.000265, 0.000005};
  p.cl3638 = {262.8, 1.7};
  p.ca_k = reduction::Measured{1.96, 0.02};
  p.cl_k = reduction::Measured{0.227, 0.004};
  arar::edit_analysis_data(a).production = p;
  const auto t = static_cast<std::int64_t>(a.timestamp);
  const std::int64_t day = 86'400;
  arar::edit_analysis_data(a).chronology = {reduction::Dose{1.0, t - 100 * day, t - 100 * day + 36'000}};
}

// A blank and a baseline on every isotope, and a detector factor on 36Ar.
inline void add_corrections(processing::Analysis& a) {
  for (processing::IsotopeData& iso : a.isotopes) {
    iso.baseline = {0.004, 0.0005};
    iso.blank = {iso.isotope == "Ar40" ? 0.8 : 0.002, iso.isotope == "Ar40" ? 0.02 : 0.0004};
    if (iso.isotope == "Ar36") iso.ic_factor = {1.012, 0.002};
    if (iso.isotope == "Ar37") iso.intercept = {0.6, 0.01};
  }
}

inline constexpr int kStepCount = 10;

// Step `step` (0 is A) of the step-heated sample: aliquot 1 of identifier
// 20001. The two first and the two last steps are older or younger than the
// six between them, which make the plateau.
inline std::shared_ptr<processing::Analysis> step_analysis(int step) {
  const std::array<double, kStepCount> ar39 = {4, 8, 30, 55, 60, 50, 35, 20, 6, 3};
  const std::array<double, kStepCount> ar36 = {0.40, 0.25, 0.12, 0.08, 0.06, 0.05, 0.06, 0.08, 0.20, 0.30};
  const std::array<double, kStepCount> f = {13.0, 11.5, 10.0, 10.0, 10.0, 10.0, 10.0, 10.0, 8.8, 8.0};
  auto a = arar::test::make_step(step, ar39[step], ar36[step], f[step], "20001");
  a->sample = "S-STEP";
  a->material = "sanidine";
  a->project = "Mapping";
  a->irradiation = "NM-900";
  a->level = "A";
  a->position = "4";
  a->extract_device = "co2";
  a->extraction.value = 0.5 + 0.5 * step;
  a->extraction.units = "W";
  a->sample_info.latitude = 37.75;
  a->sample_info.longitude = -106.9;
  a->sample_info.elevation = 2850.0;
  a->sample_info.lithology = "ash-flow tuff";
  a->sample_info.unit = "Fish Canyon Tuff";
  a->sample_info.igsn = "IEXXX0001";
  a->monitor.name = "FC-2";
  a->monitor.material = "sanidine";
  a->monitor.age = processing::Value{28.201, 0.023};
  add_corrections(*a);
  irradiate_for_real(*a);
  return a;
}

inline AnalysisList step_sample() {
  AnalysisList out;
  for (int i = 0; i < kStepCount; ++i) out.push_back(step_analysis(i));
  return out;
}

inline constexpr int kFusionCount = 6;

// Fusion `index` of the fusion sample: aliquot index + 1 of identifier 20002.
inline std::shared_ptr<processing::Analysis> fusion_analysis(int index) {
  const std::array<double, kFusionCount> ar40 = {1000.0, 1003.0, 998.0, 1001.5, 999.0, 1002.0};
  auto a = arar::test::make_unknown(index, ar40[index]);
  a->identifier = "20002";
  a->uuid = "fusion-20002-" + std::to_string(index);
  a->runid = processing::make_runid(a->identifier, a->aliquot, -1);
  a->sample = "S-FUS";
  a->material = "biotite";
  a->extraction.value = 4.0;
  a->extraction.units = "W";
  a->monitor.name = "FC-2";
  a->monitor.age = processing::Value{28.201, 0.023};
  add_corrections(*a);
  irradiate_for_real(*a);
  return a;
}

inline AnalysisList fusion_sample() {
  AnalysisList out;
  for (int i = 0; i < kFusionCount; ++i) out.push_back(fusion_analysis(i));
  return out;
}

// Both samples, the fusions first so that the order given is not the order
// of the names.
inline AnalysisList both_samples() {
  AnalysisList out = fusion_sample();
  for (const auto& a : step_sample()) out.push_back(a);
  return out;
}

// A copy of `list` with `edit` applied to analysis `index`.
template <typename Edit>
AnalysisList with_edit(const AnalysisList& list, std::size_t index, Edit edit) {
  AnalysisList out = list;
  auto copy = std::make_shared<processing::Analysis>(*list.at(index));
  edit(*copy);
  out[index] = copy;
  return out;
}

// A copy of `list` with `edit` applied to every analysis.
template <typename Edit>
AnalysisList with_edit_all(const AnalysisList& list, Edit edit) {
  AnalysisList out;
  for (const auto& a : list) {
    auto copy = std::make_shared<processing::Analysis>(*a);
    edit(*copy);
    out.push_back(copy);
  }
  return out;
}

}  // namespace pychron::earthbank::testing
