#include "pychron/reduction/flux.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace pychron::reduction {

Result<UFloat> j_of(const UFloat& f, const MonitorConstants& monitor) {
  const double f0 = f.nominal();
  if (!std::isfinite(f0) || f0 <= 0.0) return fail(ErrorKind::Config, "flux: F must be positive and finite");
  const double numerator = std::exp(monitor.lambda_k * monitor.age_a) - 1.0;
  return numerator / f;
}

std::string_view to_string(MeanKind kind) noexcept {
  switch (kind) {
    case MeanKind::Arithmetic:
      return "arithmetic";
    case MeanKind::Weighted:
      return "weighted";
  }
  return "arithmetic";
}

std::optional<MeanKind> parse_mean_kind(std::string_view text) noexcept {
  std::string s(text);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "arithmetic") return MeanKind::Arithmetic;
  if (s == "weighted") return MeanKind::Weighted;
  return std::nullopt;
}

Result<PositionMean> mean_j(std::span<const MonitorAnalysis> analyses, const MonitorConstants& monitor,
                            MeanKind kind, MeanErrorKind error) {
  std::vector<double> values, errors;
  PositionMean out;
  for (const MonitorAnalysis& a : analyses) {
    if (a.omitted) continue;
    auto j = j_of(a.f, monitor);
    const bool bad_error =
        j && kind == MeanKind::Weighted && (!std::isfinite(j->std_dev()) || j->std_dev() == 0.0);
    if (!j || bad_error) {
      out.rejected.push_back(a.record_id);
      continue;
    }
    values.push_back(j->nominal());
    errors.push_back(j->std_dev());
  }
  if (values.empty()) return fail(ErrorKind::Config, "flux: no analysis with a usable J is left");
  auto mean = kind == MeanKind::Weighted ? weighted_mean(values, errors, error)
                                         : arithmetic_mean(values, errors, error);
  if (!mean) return fail(mean.error());
  out.j = mean->value;
  out.j_err = mean->error;
  out.mswd = mean->mswd;
  out.mswd_acceptable = mean->mswd_acceptable;
  out.n = static_cast<int>(mean->n);
  return out;
}

}  // namespace pychron::reduction
