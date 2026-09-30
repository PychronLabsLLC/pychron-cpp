#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// Step letters are bijective base 26: "" -> "A", "Z" -> "AA", "AZ" -> "BA". Case-insensitive input.
std::string next_step(std::string_view step);
std::string step_name(int index);      // 0 -> "A", 26 -> "AA"
int step_index(std::string_view step);  // "A" -> 0; "" or non-letters -> -1

// Adds `by` to the trailing number, keeping prefix and zero padding: "L009" -> "L010".
// Error if there is no trailing number or the result would be negative.
Result<std::string> increment_identifier(std::string_view identifier, int by = 1);

// "<identifier>-<aliquot:02>" + step, e.g. "20001-01A".
std::string format_runid(std::string_view identifier, int aliquot, std::string_view step);
Result<RunIdentity> parse_runid(std::string_view runid);  // type left Unknown

struct IncrementOptions {
  int identifier = 0;  // added to the identifier's trailing number; specials are never incremented
  int position = 0;    // added to every hole
};

// Next run template after adding `run` (pychron's auto-increment). A changed identifier
// drops the user-fixed aliquot and step.
Result<RunSpec> next_template(const RunSpec& run, const IncrementOptions& inc,
                              const IdentifierRules& ids = IdentifierRules::defaults());

// Position-range expansion: one copy of `tmpl` per hole of `positions`.
std::vector<RunSpec> expand_positions(const RunSpec& tmpl, const Position& positions);
// Same, advancing the identifier by `identifier_step` per run.
Result<std::vector<RunSpec>> expand_positions(const RunSpec& tmpl, const Position& positions, int identifier_step);

// Step heat: one run per value with steps A, B, C, ... and extraction.value set.
std::vector<RunSpec> make_step_heat(const RunSpec& tmpl, const std::vector<double>& values);
// start, start + increment, ... (n values).
std::vector<double> step_values(double start, double increment, int n);

}  // namespace pychron::experiment
