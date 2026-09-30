#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::experiment {

// A reference to a measurable quantity. Bare isotopes and ratios resolve to the
// latest reading; series functions (min, slope, ...) read the whole series.
struct MetricRef {
  enum class Kind { Isotope, Ratio, IsotopeField, DetectorField, Age, Kca, RadiogenicYield, Device, Gauge, Param };
  Kind kind = Kind::Isotope;
  std::string a;      // isotope / numerator / detector / device / gauge / param name
  std::string b;      // ratio denominator
  std::string field;  // cur|bs|bs_corrected|ic_corrected, deflection|inactive|intensity, pressure

  friend bool operator==(const MetricRef&, const MetricRef&) = default;
};

// Text form, e.g. "Ar40/Ar39", "Ar40.bs", "gauge.ion_pump.pressure".
std::string to_string(const MetricRef& m);

// True when the metric only has meaning as a series (baseline readings). Such
// a metric may not be compared with a scalar without a reducer.
bool is_series_only(const MetricRef& m) noexcept;

enum class CmpOp { Lt, Le, Gt, Ge, Eq, Ne };
enum class Func { Min, Max, Average, Slope, Std, Rsd, Between, Count, Elapsed };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  enum class Kind { Number, Metric, Var, Call, Cmp, And, Or, Not };
  Kind kind = Kind::Number;
  double number = 0;             // Number
  MetricRef metric;              // Metric
  std::string var;               // Var ($NAME)
  Func func = Func::Min;         // Call
  std::optional<int> window;     // Call: last N points of a series argument
  CmpOp op = CmpOp::Lt;          // Cmp
  std::vector<ExprPtr> children; // Call args / Cmp(l,r) / And,Or(l,r) / Not(x)
};

// Parses one check string into an AST, validating function arities, argument
// kinds and series-vs-scalar use. `text` uses the grammar of spec section 7.
Result<ExprPtr> parse_expression(std::string_view text);

// Canonical, re-parseable text of an AST.
std::string to_string(const Expr& e);
std::string_view to_string(Func f) noexcept;
std::string_view to_string(CmpOp op) noexcept;

}  // namespace pychron::experiment
