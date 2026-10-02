#pragma once

// Conditional check expressions (conditionals spec section 3).
//
//   expr     := or_expr
//   or_expr  := and_expr ('or' and_expr)*
//   and_expr := not_expr ('and' not_expr)*
//   not_expr := 'not' not_expr | cmp
//   cmp      := sum (op sum)?                      op in < <= > >= == !=
//   sum      := term (('+'|'-') term)*
//   term     := unary (('*'|'/') unary)*
//   unary    := '-' unary | atom
//   atom     := number | metric | func '(' args ')' | '$' NAME | '(' expr ')'
//
// `ISO/ISO` with two bare names is an isotope ratio metric (usable as a
// series); any other '/' is arithmetic.

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::experiment {

// A reference to a measurable quantity. Bare isotopes and ratios resolve to the
// live corrected value; series functions (min, slope, ...) read raw series.
struct MetricRef {
  enum class Kind { Isotope, Ratio, IsotopeField, DetectorField, Computed, Device, Gauge, Param };
  Kind kind = Kind::Isotope;
  std::string a;      // isotope / numerator / detector / computed / device / gauge / param name
  std::string b;      // ratio denominator
  std::string field;  // IsotopeField: cur|bs|bs_corrected|ic_corrected|intercept|std_dev
                      // DetectorField: deflection|inactive|intensity; Gauge: pressure

  friend bool operator==(const MetricRef&, const MetricRef&) = default;
};

// Text form, e.g. "Ar40/Ar39", "Ar40.bs", "L2(CDD).deflection", "gauge.ion_pump.pressure".
std::string to_string(const MetricRef& m);

// True when the metric only has meaning as a series (baseline readings). Such
// a metric may not be compared with a scalar without a reducer.
bool is_series_only(const MetricRef& m) noexcept;

// Names parsed as MetricRef::Kind::Computed: age, instant_age, kca, cak, kcl,
// clk, radiogenic_yield, rad40, rad40_percent, atm40, k39, ca37, ca39, ca36, cl36.
std::span<const std::string_view> computed_metric_names() noexcept;

enum class CmpOp { Lt, Le, Gt, Ge, Eq, Ne };
enum class Func { Min, Max, Average, Slope, Std, Rsd, Between, Count, Elapsed, Abs };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  enum class Kind { Number, Metric, Var, Call, Cmp, And, Or, Not, Neg, Add, Sub, Mul, Div };
  Kind kind = Kind::Number;
  double number = 0;             // Number
  MetricRef metric;              // Metric
  std::string var;               // Var ($NAME)
  Func func = Func::Min;         // Call
  std::optional<int> window;     // Call: last N points of a series argument
  CmpOp op = CmpOp::Lt;          // Cmp
  std::vector<ExprPtr> children; // Call args / Cmp, And, Or, Add.. (l, r) / Not, Neg (x)
};

// Parses one check string into an AST, validating function arities, argument
// kinds and series-vs-scalar use.
Result<ExprPtr> parse_expression(std::string_view text);

// Canonical, re-parseable text of an AST.
std::string to_string(const Expr& e);
std::string_view to_string(Func f) noexcept;
std::string_view to_string(CmpOp op) noexcept;

ExprPtr clone(const Expr& e);

// Every metric the expression reads, in first-use order, without duplicates.
std::vector<MetricRef> metrics_of(const Expr& e);
// Every $NAME the expression reads.
std::vector<std::string> variables_of(const Expr& e);

// Legacy `window` (spec L7): bare isotope metrics become average(ISO, window=N)
// and series functions without their own window get N.
ExprPtr apply_window(const Expr& e, int window);

// Legacy `mapper` (spec L8): every metric and series-function node M is
// replaced by mapper[x := M]. `mapper` is an arithmetic expression in `x`.
Result<ExprPtr> apply_mapper(const Expr& e, std::string_view mapper);

}  // namespace pychron::experiment
