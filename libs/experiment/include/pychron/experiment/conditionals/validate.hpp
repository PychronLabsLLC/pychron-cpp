#pragma once

// Static validation of conditionals against what a lab has (conditionals
// spec section 7.3): every metric name must exist, computed values must be
// computable, and pre-run checks cannot read isotope data (there is none
// before measurement). An empty catalog set means "not checked".

#include <set>
#include <string>
#include <vector>

#include "pychron/experiment/conditionals/conditional.hpp"

namespace pychron::experiment {

struct MetricCatalog {
  std::set<std::string> isotopes;   // e.g. Ar36..Ar40
  std::set<std::string> detectors;  // spectrometer.toml
  std::set<std::string> gauges;     // extraction_line.toml
  std::set<std::string> devices;    // device readers the lab provides
  std::set<std::string> variables;  // $NAMEs that will be defined
  bool computed = false;            // Ar-Ar constants are configured (age, kca, ...)
  bool chlorine = false;            // live chlorine corrections are configured (kcl, clk, cl36)
};

struct ConditionalDiagnostic {
  std::string conditional;  // name
  std::string message;
  bool error = true;        // false: warning
};

std::vector<ConditionalDiagnostic> validate_conditionals(const ConditionalSet& set, const MetricCatalog& catalog);

}  // namespace pychron::experiment
