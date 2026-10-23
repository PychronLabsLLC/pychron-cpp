#pragma once

// The processing tests' synthetic analyses (tests/processing fixtures.hpp)
// with Ar-Ar data, as the Ar-Ar loaders give every analysis they load: an air
// with none of its parts, an unknown and a heating step irradiated.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "../processing/fixtures.hpp"
#include "pychron/arar/analysis_data.hpp"

namespace pychron::arar::test {

// Ar-Ar data without a flux, production or chronology: what the loaders give
// an air.
inline void unirradiated(processing::Analysis& a) { a.extension = std::make_shared<const ArArAnalysisData>(); }

// An unknown needs production ratios and a chronology to have an age. These
// add no correction: zero interference ratios and a dose of zero power, a day
// before the run, so the decay factors are exactly 1.
inline void irradiate(processing::Analysis& a) {
  auto data = std::make_shared<ArArAnalysisData>();
  data->flux = reduction::Flux{{0.001, 1e-6}, 0.0, std::nullopt};
  data->production = reduction::ProductionRatios{};
  const auto t = static_cast<std::int64_t>(a.timestamp);
  data->chronology = {reduction::Dose{0.0, t - 86'400, t - 82'800}};
  a.extension = std::move(data);
}

inline std::shared_ptr<processing::Analysis> make_air(int index, double ratio = 295.5, std::string type = "air",
                                                      std::string identifier = "A1") {
  auto a = processing::test::make_air(index, ratio, std::move(type), std::move(identifier));
  unirradiated(*a);
  return a;
}

// An unknown with a flux, so it has an age.
inline std::shared_ptr<processing::Analysis> make_unknown(int index, double ar40 = 1000.0) {
  auto a = processing::test::make_unknown(index, ar40);
  irradiate(*a);
  return a;
}

inline std::shared_ptr<processing::Analysis> make_step(int step, double ar39, double ar36, double f = 10.0,
                                                       std::string identifier = "S1") {
  auto a = processing::test::make_step(step, ar39, ar36, f, std::move(identifier));
  irradiate(*a);
  return a;
}

}  // namespace pychron::arar::test
