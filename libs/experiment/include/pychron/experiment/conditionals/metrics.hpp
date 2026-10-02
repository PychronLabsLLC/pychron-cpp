#pragma once

// MetricContext implementations shared by the run layers (conditionals spec
// section 5). The in-run Collector context lives in collect/collector.hpp and
// the instrument adapter in measurement/adapters.hpp.

#include <optional>
#include <string>
#include <vector>

#include "pychron/experiment/conditionals/evaluator.hpp"
#include "pychron/experiment/record/types.hpp"
#include "pychron/reduction/arar.hpp"

namespace pychron::experiment {

// The first context that answers wins.
class ChainContext final : public MetricContext {
 public:
  ChainContext() = default;
  explicit ChainContext(std::vector<const MetricContext*> chain) : chain_(std::move(chain)) {}
  void add(const MetricContext* ctx) { chain_.push_back(ctx); }

  std::optional<std::vector<double>> series(const MetricRef& m) const override;
  std::optional<double> scalar(const MetricRef& m) const override;
  std::optional<double> elapsed() const override;

 private:
  std::vector<const MetricContext*> chain_;
};

// Post-run checks: the same metric names over a finished AnalysisRecord.
//   Ar40              (intercept - baseline) * icfactor, from results
//   Ar40.intercept / .std_dev / .bs_corrected / .ic_corrected / .cur
//   Ar40.bs           the detector's recorded baseline points
//   Ar40/Ar39, age, kca, ...   as in-run (computed values need constants)
// An isotope measured on several detectors is looked up as "Ar36:CDD" first
// by its most-populated signal series. elapsed() is unavailable.
class RecordMetrics final : public MetricContext {
 public:
  explicit RecordMetrics(const record::AnalysisRecord& record,
                         std::optional<reduction::ArArConstants> arar = std::nullopt)
      : rec_(record), arar_(std::move(arar)) {}

  std::optional<std::vector<double>> series(const MetricRef& m) const override;
  std::optional<double> scalar(const MetricRef& m) const override;
  std::optional<double> elapsed() const override { return std::nullopt; }

 private:
  const record::DataSeries* signal(const std::string& iso) const;
  const record::InterceptResult* intercept(const std::string& iso, std::string* det) const;
  std::optional<double> corrected(const std::string& iso) const;

  const record::AnalysisRecord& rec_;
  std::optional<reduction::ArArConstants> arar_;
};

}  // namespace pychron::experiment
