#include "pychron/systems/spectrometer/corrections.hpp"

#include <cmath>
#include <string>

namespace pychron::spectrometer {

double deflection_offset(const CorrectionInputs& in) noexcept {
  if (!in.deflection_enabled) return 0.0;
  double sum = 0.0;
  for (auto it = in.deflection_poly.rbegin(); it != in.deflection_poly.rend(); ++it) sum = sum * in.deflection + *it;
  return static_cast<double>(in.deflection_sign) * sum;
}

Result<double> hv_factor(const CorrectionInputs& in) {
  if (!in.hv_enabled || in.axis != IMassPositioner::Axis::Dac) return 1.0;
  if (!(in.hv_actual > 0.0) || !(in.hv_nominal > 0.0)) {
    return fail(ErrorKind::Config, "HV correction needs positive HV (actual " + std::to_string(in.hv_actual) +
                                       ", nominal " + std::to_string(in.hv_nominal) + ")");
  }
  return std::sqrt(in.hv_actual / in.hv_nominal);
}

Result<double> correct(double table_value, const CorrectionInputs& in) {
  auto factor = hv_factor(in);
  if (!factor) return fail(factor.error());
  return (table_value + deflection_offset(in)) * *factor;
}

Result<double> uncorrect(double native_value, const CorrectionInputs& in) {
  auto factor = hv_factor(in);
  if (!factor) return fail(factor.error());
  return native_value / *factor - deflection_offset(in);
}

}  // namespace pychron::spectrometer
