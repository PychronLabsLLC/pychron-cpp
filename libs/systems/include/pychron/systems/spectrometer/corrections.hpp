#pragma once

// Position corrections (spectrometer spec section 4.2, step 3). Pure: the
// caller supplies the detector's deflection and the HV readback.
//
//   native = (table + sign * poly(deflection)) * sqrt(hv_actual / hv_nominal)
//
// Each term is toggleable; the HV term only applies to Axis::Dac. uncorrect()
// is the exact inverse of correct().

#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer {

struct CorrectionInputs {
  IMassPositioner::Axis axis = IMassPositioner::Axis::Dac;

  bool deflection_enabled = false;
  std::vector<double> deflection_poly;  // lowest order first
  int deflection_sign = 1;
  double deflection = 0.0;

  bool hv_enabled = false;
  double hv_actual = 0.0;
  double hv_nominal = 0.0;
};

// Additive deflection term: sign * poly(deflection); 0 when disabled.
double deflection_offset(const CorrectionInputs& in) noexcept;

// Multiplicative HV factor sqrt(actual / nominal); 1 when disabled or not
// Axis::Dac. Config error for non-positive HV values.
Result<double> hv_factor(const CorrectionInputs& in);

// Table value -> native value.
Result<double> correct(double table_value, const CorrectionInputs& in);
// Native value -> table value; exact inverse of correct().
Result<double> uncorrect(double native_value, const CorrectionInputs& in);

}  // namespace pychron::spectrometer
