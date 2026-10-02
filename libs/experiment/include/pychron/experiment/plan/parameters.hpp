#pragma once

// Plan parameters for editing a run's measurement (experiment-window design,
// measurement panel): what a template lets a run change, what each value is
// in the template, and the rules for typed text and overrides.
//
// A run's overrides set template paths (render_effective_plan). Normally only
// the template's parameters.expose paths (and keys under an exposed table) may
// be set; a run marked `advanced` may set any existing key.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/identifiers.hpp"
#include "pychron/experiment/model/types.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace pychron::experiment::plan {

enum class ParamKind {
  Bool,
  Int,
  Float,
  String,
  List,   // array of strings, edited as a comma-separated list
  Alias,  // the template holds an '@' alias; any scalar may replace it
};
std::string_view to_string(ParamKind kind) noexcept;

struct PlanParameter {
  std::string path;   // "main.hops[0].counts"
  std::string label;  // expose label, else the path
  ParamKind kind = ParamKind::String;
  ParamValue value;   // the template's value (List: "a, b"; Alias: "@extraction.eqtime")
  bool exposed = false;
  friend bool operator==(const PlanParameter&, const PlanParameter&) = default;
};

// `all` false: the exposed parameters in parameters.expose order; an exposed
// table contributes every value under it. `all` true: every editable value of
// the template (bools, numbers, strings, string lists) outside [plan] and
// [parameters], in path order, exposed ones marked. Arrays of numbers and
// empty tables are left out.
Result<std::vector<PlanParameter>> plan_parameters(const PlanTemplate& tmpl, bool all = false);

// The template's [plan] table (name, instrument_family, description, analysis_types).
PlanInfo plan_info(const PlanTemplate& tmpl);

// Plans whose instrument family is `family` (empty: any) and whose
// analysis_types include `type` (a plan listing none takes every type). Sorted.
std::vector<std::string> matching_plans(const PlanLibrary& plans, std::string_view family, AnalysisType type);
// Every instrument family the library's plans declare, sorted, without empties.
std::vector<std::string> plan_families(const PlanLibrary& plans);

// Typed value from form text. Bool: true/false/yes/no/1/0; Int; Float (an
// integer is fine); String and List: the text; Alias: a number, true/false, or
// the text. nullopt when the text does not fit.
std::optional<ParamValue> parse_param(ParamKind kind, std::string_view text);
std::string format_param(const ParamValue& value);
// Equal values, with an int and a float comparing numerically.
bool same_param(const ParamValue& a, const ParamValue& b);

// The overrides that still apply to `tmpl` (each renders on its own), for
// moving a run to another plan.
ParamOverrides overrides_for(const PlanTemplate& tmpl, const ParamOverrides& overrides, bool advanced);

}  // namespace pychron::experiment::plan
