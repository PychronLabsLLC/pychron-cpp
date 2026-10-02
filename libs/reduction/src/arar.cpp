#include "pychron/reduction/arar.hpp"

#include <cmath>

namespace pychron::reduction {

std::map<std::string, double> compute_arar(const ArArIntensities& in, const ArArConstants& c) {
  std::map<std::string, double> out;
  const double ca37 = in.ar37.value_or(0.0) * c.df37;
  const double ca36 = c.ca3637 * ca37;
  const double ca39 = c.ca3937 * ca37;
  if (in.ar37) {
    out["ca37"] = ca37;
    out["ca36"] = ca36;
    out["ca39"] = ca39;
  }
  std::optional<double> k39;
  if (in.ar39) {
    k39 = *in.ar39 * c.df39 - ca39;
    out["k39"] = *k39;
  }
  std::optional<double> atm40;
  if (in.ar36) {
    atm40 = (*in.ar36 - ca36) * c.atm4036;
    out["atm40"] = *atm40;
  }
  if (in.ar40 && atm40) {
    const double rad40 = *in.ar40 - *atm40 - c.k4039 * k39.value_or(0.0);
    out["rad40"] = rad40;
    if (*in.ar40 != 0) out["radiogenic_yield"] = out["rad40_percent"] = 100.0 * rad40 / *in.ar40;
    if (k39 && *k39 != 0 && c.j > 0 && c.lambda_total > 0) {
      const double arg = 1.0 + c.j * rad40 / *k39;
      if (arg > 0) out["age"] = std::log(arg) / c.lambda_total / 1e6;
    }
  }
  if (k39 && in.ar37 && ca37 != 0 && *k39 != 0) {
    out["kca"] = c.kca_factor * *k39 / ca37;
    out["cak"] = 1.0 / out["kca"];
  }
  return out;
}

}  // namespace pychron::reduction
