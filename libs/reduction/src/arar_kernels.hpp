// Shared Ar-Ar kernels (spec 8.1). Private header.
//
// Each equation is written once as a function template over the number type
// T and instantiated for UFloat (reduce() and the step functions in
// arar_reduction.hpp) and double (compute_arar, live conditionals), so the two
// paths cannot drift. Kernels hold arithmetic only: no validation, no
// Result, no diagnostics; callers check domains first. Branches and zero tests
// inside a kernel go through nominal(), which drops the uncertainty and is the
// identity for double. Arithmetic is written in the legacy operand order so
// the UFloat path reproduces `uncertainties` to rounding (spec 4.7); the
// library builds with -ffp-contract=off so the double path never fuses a * b + c.
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

// The interference ratios E9-E11 read (missing = exact 0).
template <class T>
struct InterferenceRatios {
  T k3739, k3839, ca3937, ca3837, ca3637;
};

// E9-E11 outputs plus the branches taken, so callers can raise diagnostics
// (the kernel itself emits none).
template <class T>
struct Interference {
  T k37, k38, k39, ca36, ca37, ca38, ca39;
  bool fixed_zero_ca3937 = false;  // E10 took y = 1
  bool ca_clamped = false;         // E11 set ca37 to exact 0
};

// Python truthiness of a legacy fixed_k3739 value (`not fixed_k3739` at
// argon_calculations.py:410, :416): false only for 0 +- 0.
inline bool truthy(double x) noexcept { return x != 0.0; }
inline bool truthy(const UFloat& x) noexcept { return !(x.nominal() == 0.0 && x.std_dev() == 0.0); }

// E9-E11. `fixed_k3739` null selects normal mode (E9); otherwise E10 with that
// x. The E11 clamp runs after ca39 and k39 were computed from the unclamped
// ca37 (spec Q8) and yields an exact 0 (Python max(ufloat(0, 0), ca37) keeps
// the first argument unless ca37 > 0, so NaN and 0 are clamped too).
// legacy:processing/argon_calculations.py:375-426
template <class T>
Interference<T> interference(const T& a39, const T& a37, const InterferenceRatios<T>& r,
                             const T* fixed_k3739, bool allow_negative_ca_correction) {
  Interference<T> out;
  if (fixed_k3739 == nullptr) {
    // E9 (:410-414)
    out.k39 = (a39 - r.ca3937 * a37) / (1.0 - r.k3739 * r.ca3937);
    out.k37 = r.k3739 * out.k39;
    out.ca37 = a37 - out.k37;
    out.ca39 = r.ca3937 * out.ca37;
  } else {
    // E10 (:375-396): y = 1 / Ca3937, or 1 when that division would raise.
    const T& x = *fixed_k3739;
    T y = 1.0;
    if (nominal(r.ca3937) == 0.0) {
      out.fixed_zero_ca3937 = true;
    } else {
      y = 1.0 / r.ca3937;
    }
    out.ca37 = (a39 * x * y) / (x + y);
    out.ca39 = r.ca3937 * out.ca37;
    out.k39 = a39 - out.ca39;
    out.k37 = x * out.k39;
  }
  // E11 (:420-424)
  out.k38 = r.k3839 * out.k39;
  if (!allow_negative_ca_correction && !(nominal(out.ca37) > 0.0)) {
    out.ca37 = T(0.0);
    out.ca_clamped = true;
  }
  out.ca36 = r.ca3637 * out.ca37;
  out.ca38 = r.ca3837 * out.ca37;
  return out;
}

// E12 outputs. `singular` means nom(1 - m r3836) == 0 (legacy
// ZeroDivisionError); the components are then left default (exact 0).
template <class T>
struct Atmospheric {
  T atm36, atm38, cl36, cl38;
  bool singular = false;
};

// E12 with the legacy operand order: m = (Cl3638 * lCl) * dd, then
// atm36 = ((a36 - ca36) - m ((a38 - k38) - ca38)) / (1 - m r3836),
// atm38 = r3836 atm36, cl38 = ((a38 - atm38) - k38) - ca38, cl36 = cl38 m.
// legacy:processing/argon_calculations.py:481-485
template <class T>
Atmospheric<T> atmospheric(const T& a38, const T& a36, const T& k38, const T& ca38, const T& ca36,
                           double decay_days, const T& cl3638, const T& lambda_cl36,
                           const T& r3836) {
  Atmospheric<T> out;
  const T m = cl3638 * lambda_cl36 * decay_days;
  const T denom = 1.0 - m * r3836;
  if (nominal(denom) == 0.0) {
    out.singular = true;
    return out;
  }
  out.atm36 = (a36 - ca36 - m * (a38 - k38 - ca38)) / denom;
  out.atm38 = r3836 * out.atm36;
  out.cl38 = a38 - out.atm38 - k38 - ca38;
  out.cl36 = out.cl38 * m;
  return out;
}

// E13 outputs. `singular` means nom(c36) == 0 or nom(rc - rs) == 0 (legacy
// ZeroDivisionError); the components are then left default (exact 0).
template <class T>
struct Cosmogenic {
  T cosmo36, cosmo38, noncosmo36, noncosmo38;
  bool singular = false;
};

// E13. legacy:processing/argon_calculations.py:501-511
template <class T>
Cosmogenic<T> cosmogenic(const T& c36, const T& c38, const T& rs, const T& rc) {
  Cosmogenic<T> out;
  if (nominal(c36) == 0.0) {
    out.singular = true;
    return out;
  }
  const T rm = c38 / c36;
  const T spread = rc - rs;
  if (nominal(spread) == 0.0) {
    out.singular = true;
    return out;
  }
  const T fs = (rc - rm) / spread;
  const T fc = 1.0 - fs;
  out.noncosmo38 = fs * c38;
  out.cosmo38 = c38 - out.noncosmo38;
  out.cosmo36 = fc * c36;
  out.noncosmo36 = c36 - out.cosmo36;
  return out;
}

// E14 outputs. `f` and `yield` are meaningful only when the matching flag is
// set (legacy ZeroDivisionError sentinels become absent values, D3).
template <class T>
struct FValues {
  T atm40, k40, rad40, f, yield;
  bool f_defined = false;      // nom(k39) != 0
  bool yield_defined = false;  // nom(n40) != 0
};

// E14 with the legacy operand order: atm40 = atm36 * T, k40 = k39 * K4039,
// rad40 = (n40 - atm40) - k40, F = rad40 / k39, yield = (rad40 / n40) * 100.
// legacy:processing/argon_calculations.py:547-557
template <class T>
FValues<T> f_and_yield(const T& n40, const T& k39, const T& atm36, const T& trapped_4036,
                       const T& k4039) {
  FValues<T> out;
  out.atm40 = atm36 * trapped_4036;
  out.k40 = k39 * k4039;
  out.rad40 = n40 - out.atm40 - out.k40;
  if (nominal(k39) != 0.0) {
    out.f = out.rad40 / k39;
    out.f_defined = true;
  }
  if (nominal(n40) != 0.0) {
    out.yield = out.rad40 / n40 * 100.0;
    out.yield_defined = true;
  }
  return out;
}

}  // namespace pychron::reduction::kernels
