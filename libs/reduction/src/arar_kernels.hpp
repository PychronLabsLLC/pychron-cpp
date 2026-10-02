// Shared Ar-Ar kernels (spec 8.1). Private header.
//
// Each equation is written once as a function template over the number type
// T and instantiated for UFloat (reduce() and the step functions in
// arar_reduction.hpp) and double (compute_arar, live conditionals), so the two
// paths cannot drift. Kernels hold arithmetic only: no validation, no
// Result, no diagnostics; callers check domains first. Branches and zero tests
// inside a kernel go through nominal(), which drops the uncertainty and is the
// identity for double. Arithmetic is written in the legacy operand order so
// the UFloat path reproduces `uncertainties` to rounding (spec 4.7).
#pragma once

#include <array>

#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction::kernels {

inline double nominal(double x) noexcept { return x; }
inline double nominal(const UFloat& x) noexcept { return x.nominal(); }

// E4 abundance sensitivity over ARGON_KEYS order (Ar40, Ar39, Ar38, Ar37,
// Ar36). Assumes a symmetric, equal tail on every peak; the 40 and 36 peaks
// take 2*s39 and 2*s37 because 41 and 35 are not measured (spec Q9).
// legacy:processing/argon_calculations.py:363-372
template <class T>
std::array<T, 5> abundance_sensitivity(const std::array<T, 5>& s, double alpha) {
  const T& s40 = s[0];
  const T& s39 = s[1];
  const T& s38 = s[2];
  const T& s37 = s[3];
  const T& s36 = s[4];
  return {s40 - alpha * (s39 + s39), s39 - alpha * (s40 + s38), s38 - alpha * (s39 + s37),
          s37 - alpha * (s38 + s36), s36 - alpha * (s37 + s37)};
}

// E5 deadtime on a signal in fA: to counts/s, n / (1 - n tau), back to fA.
// The caller guarantees 1 - n tau > 0 (spec 6).
// legacy:processing/deadtime.py:54-55
template <class T>
T deadtime(const T& signal_fa, double tau_s, double fa_to_cps) {
  const T n = signal_fa * fa_to_cps;
  return (n / (1.0 - n * tau_s)) / fa_to_cps;
}

}  // namespace pychron::reduction::kernels
