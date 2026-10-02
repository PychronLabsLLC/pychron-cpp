#pragma once

// Synthetic analyses for the processing tests.

#include <memory>
#include <string>

#include "pychron/processing/model.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing::test {

// An air-like analysis: Ar40/Ar36 = ratio, Ar36 = 10 fA, small baseline and
// blank, one hour apart per index.
inline std::shared_ptr<Analysis> make_air(int index, double ratio = 295.5, std::string type = "air",
                                          std::string identifier = "A1") {
  auto a = std::make_shared<Analysis>();
  a->uuid = "uuid-" + identifier + "-" + std::to_string(index);
  a->identifier = identifier;
  a->aliquot = index + 1;
  a->runid = make_runid(identifier, a->aliquot, -1);
  a->analysis_type = std::move(type);
  a->timestamp = 1'700'000'000.0 + 3600.0 * index;
  a->mass_spectrometer = "jan";
  a->sample = "air";
  const double ar36 = 10.0;
  auto iso = [&](const char* name, const char* det, double v, double e) {
    IsotopeData d;
    d.key = name;
    d.isotope = name;
    d.detector = det;
    d.intercept = {v, e};
    d.baseline = {0.01, 0.001};
    d.blank = {0.0, 0.0};
    d.n = 100;
    a->isotopes.push_back(d);
  };
  iso("Ar40", "H1", ar36 * ratio + 0.01, 0.5);
  iso("Ar39", "AX", 1.0, 0.01);
  iso("Ar38", "L1", 2.0, 0.01);
  iso("Ar37", "L2", 0.5, 0.01);
  iso("Ar36", "CDD", ar36 + 0.01, 0.02);
  a->gains = {{"H1", 1.01}, {"AX", 1.0}};
  a->extraction.value = 5.0 + index;
  a->extraction.units = "W";
  return a;
}

// An unknown with a flux, so it has an age.
inline std::shared_ptr<Analysis> make_unknown(int index, double ar40 = 1000.0) {
  auto a = make_air(index, 295.5, "unknown", "U1");
  a->isotopes[0].intercept = {ar40, 0.5};   // Ar40
  a->isotopes[1].intercept = {100.0, 0.1};  // Ar39
  a->isotopes[4].intercept = {0.5, 0.01};   // Ar36
  a->context.flux = reduction::Flux{{0.001, 1e-6}, 0.0, std::nullopt};
  return a;
}

// One heating step of aliquot 1 of `identifier`: Ar40 = F Ar39 + 298.56 Ar36
// (the Default preset's atmospheric ratio, no interferences), so every step
// has the same F and age and the points lie on one inverse isochron.
inline std::shared_ptr<Analysis> make_step(int step, double ar39, double ar36, double f = 10.0,
                                           std::string identifier = "S1") {
  auto a = std::make_shared<Analysis>();
  a->uuid = "step-" + identifier + "-" + std::to_string(step);
  a->identifier = identifier;
  a->aliquot = 1;
  a->increment = step;
  a->runid = make_runid(identifier, 1, step);
  a->analysis_type = "unknown";
  a->timestamp = 1'700'000'000.0 + 1800.0 * step;
  a->sample = "FC-2";
  a->mass_spectrometer = "jan";
  auto iso = [&](const char* name, double v, double e) {
    IsotopeData d;
    d.key = name;
    d.isotope = name;
    d.intercept = {v, e};
    a->isotopes.push_back(d);
  };
  iso("Ar40", f * ar39 + 298.56 * ar36, 0.02 * (f * ar39 + 298.56 * ar36) / 100.0 + 0.05);
  iso("Ar39", ar39, ar39 * 0.002 + 0.01);
  iso("Ar38", 0.2 * ar36 + 0.01, 0.005);
  iso("Ar37", 0.1, 0.005);
  iso("Ar36", ar36, ar36 * 0.01 + 0.001);
  a->context.flux = reduction::Flux{{0.001, 1e-6}, 0.0, std::nullopt};
  return a;
}

}  // namespace pychron::processing::test
