#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/experiment/conditionals/expr.hpp"

namespace pychron::experiment {

// Typed source of metric values. Backed by the Collector series, the
// spectrometer snapshot, the extraction line and reduction computations.
// Return nullopt when a metric is unavailable.
class MetricContext {
 public:
  virtual ~MetricContext() = default;
  virtual std::optional<std::vector<double>> series(const MetricRef& m) const = 0;
  virtual std::optional<double> scalar(const MetricRef& m) const = 0;
  virtual std::optional<double> elapsed() const = 0;  // seconds since measurement start
};

// $NAME resolution: script_options first, then plan parameters.
struct Variables {
  std::map<std::string, double> script_options;
  std::map<std::string, double> params;
  std::optional<double> lookup(std::string_view name) const;
};

// Evaluates to a number; comparisons and boolean operators yield 0 or 1.
// Errors: missing metric/variable, empty series.
Result<double> evaluate(const Expr& e, const MetricContext& ctx, const Variables& vars);

// Truthiness of a check; for a top-level comparison `value` is its left operand.
struct CheckResult {
  bool tripped = false;
  double value = 0;
};
Result<CheckResult> evaluate_check(const Expr& e, const MetricContext& ctx, const Variables& vars);

// Simple in-memory context, for tests and replays.
class MapContext final : public MetricContext {
 public:
  std::map<std::string, std::vector<double>> series_data;  // keyed by to_string(MetricRef)
  std::optional<double> elapsed_s;

  std::optional<std::vector<double>> series(const MetricRef& m) const override;
  std::optional<double> scalar(const MetricRef& m) const override;
  std::optional<double> elapsed() const override { return elapsed_s; }
};

}  // namespace pychron::experiment
