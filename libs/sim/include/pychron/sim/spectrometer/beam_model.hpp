#pragma once

// BeamModel: the simulated ion beam behind every spectrometer sim driver
// (spectrometer spec section 8.1). One instance is shared by the drivers of a
// config, so a magnet move made through the positioner shows up in the
// intensities read through the acquirer.
//
// Physics, deliberately simple:
//   - Gas: isotope -> mass and abundance (signal at the peak top, fA), with
//     optional exponential decay/growth. Nothing sits at mass 34.2, so the
//     baseline there is zero plus noise.
//   - Peak: per detector and isotope a flat-top trapezoid centred at
//       table_value(mass, det) * sqrt(HV / nominal_hv) + shift
//     where shift = deflection polynomial + geometry offset + symmetry shift.
//   - Sensitivity scales with trap current, extraction focus (Gaussian around
//     an optimum), detector gain and, for counters, the CDD voltage plateau.
//   - Noise: Gaussian for Faraday, Poisson with non-paralyzable dead time for
//     counters (returned as cps). Values clamp at `saturation`.
//   - An unprotected counter seeing more than its overload threshold latches
//     `overloaded`; a protected detector or a blanked beam sees nothing.
// Time comes from the injected Clock, randomness from a seeded generator, so
// tests are deterministic. All methods are thread-safe.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/detectors.hpp"
#include "pychron/devices/spectrometer/params.hpp"

namespace pychron::sim {

struct BeamGas {
  std::string isotope;
  double mass = 0.0;
  double abundance = 0.0;           // signal at the peak top, fA
  double rate_per_s = 0.0;          // abundance *= exp(rate * t); negative decays
};

struct BeamDetector {
  std::string name;  // acquirer channel id
  spectrometer::DetectorKind kind = spectrometer::DetectorKind::Faraday;
  double offset = 0.0;                    // geometry offset, table units
  std::vector<double> deflection_correction{0.0, 0.0012};  // poly of deflection, lowest first
  int deflection_sign = 1;
  double gain = 1.0;
  double deflection = 0.0;
  double cdd_voltage = 1450.0;
  double dead_time_ns = 25.0;
  double sensitivity = 1.0;
  double noise_floor = 1.0;               // Faraday Gaussian sigma at zero signal, fA
  double noise_rel = 0.001;               // Faraday Gaussian sigma per unit signal
  double saturation = 4.9e6;
  double overload_threshold = 5e5;        // cps an unprotected counter tolerates
  bool protect = false;
  bool overloaded = false;                // latched
};

// One intensity draw. `value` is fA for Faraday, cps for counters.
struct BeamIntensity {
  double value = 0.0;
  bool saturated = false;
  bool overloaded = false;
};

struct BeamSettings {
  std::uint64_t seed = 0x5eed;
  double nominal_hv = 4500.0;
  double flat_half_width = 0.02;          // table units
  double edge_width = 0.01;               // table units, linear ramp to zero
  std::vector<BeamGas> gas;               // empty: argon defaults
  // (mass, detector) -> peak-centre magnet value before HV/deflection.
  std::function<double(double mass, const std::string& detector)> table_value;
  double nominal_trap_current = 100.0;
  double symmetry_shift_per_unit = 0.001;   // YSymmetry / ZSymmetry shift the peak
  double extraction_focus_optimum = 0.0;
  double extraction_focus_width = 20.0;
  double cdd_plateau_center = 1200.0;
  double cdd_plateau_width = 60.0;
};

// Argon isotopes 36..40 with sim-friendly abundances.
std::vector<BeamGas> default_argon_gas();
// Default table: mass / 5, i.e. Ar40 at 8.0 on a 0..10 DAC.
double default_table_value(double mass, const std::string& detector);
// Counter kinds for "CDD", "EM", "IC*", "counter*"; Faraday otherwise.
spectrometer::DetectorKind infer_detector_kind(std::string_view name);

class BeamModel {
 public:
  explicit BeamModel(const Clock& clock, BeamSettings settings = {});

  const Clock& clock() const noexcept { return clock_; }
  double nominal_hv() const noexcept { return settings_.nominal_hv; }

  // Adds a detector (or replaces it, keeping runtime state) and returns it.
  void add_detector(BeamDetector detector);
  // Adds a default detector of inferred kind if absent.
  void ensure_detector(std::string_view name);
  bool has_detector(std::string_view name) const;
  Result<BeamDetector> detector(std::string_view name) const;

  void set_gas(std::vector<BeamGas> gas);

  // Magnet position in table units.
  void set_magnet(double value);
  double magnet() const;
  void set_hv(double volts);
  double hv() const;

  void set_source_param(const spectrometer::ParamId& id, double value);
  double source_param(const spectrometer::ParamId& id) const;

  Result<void> set_deflection(std::string_view det, double value);
  Result<void> set_gain(std::string_view det, double value);
  Result<void> set_cdd_voltage(std::string_view det, double volts);
  Result<void> protect(std::string_view det, bool on);
  void blank(bool on);
  bool blanked() const;
  bool overloaded(std::string_view det) const;
  Result<void> clear_overload(std::string_view det);

  // Peak-centre magnet value of `isotope` on `det` under current state.
  Result<double> peak_center(std::string_view det, std::string_view isotope) const;

  // Intensity at instant `t`; counters return cps averaged over `gate`.
  Result<BeamIntensity> intensity(std::string_view det, TimePoint t, Duration gate = std::chrono::seconds(1));
  Result<BeamIntensity> intensity(std::string_view det, Duration gate = std::chrono::seconds(1));

 private:
  const BeamDetector* find_locked(std::string_view name) const;
  BeamDetector* find_locked(std::string_view name);
  double param_locked(const spectrometer::ParamId& id) const;
  double center_locked(const BeamDetector& d, double mass) const;
  double shift_locked(const BeamDetector& d) const;
  double sensitivity_locked(const BeamDetector& d) const;
  double shape_locked(double magnet, double center) const;
  double true_signal_locked(const BeamDetector& d, TimePoint t) const;

  const Clock& clock_;
  BeamSettings settings_;
  TimePoint t0_;
  mutable std::mutex mutex_;
  std::vector<BeamDetector> detectors_;
  std::map<spectrometer::ParamId, double> params_;
  double magnet_ = 0.0;
  double hv_ = 0.0;
  bool blank_ = false;
  std::mt19937_64 rng_;
};

// Process-wide named models so drivers built by DriverRegistry from separate
// [drivers.*] tables bind to the same beam. Drivers read the `beam` option
// (default "default").
class BeamModelRegistry {
 public:
  static BeamModelRegistry& global();

  // The model called `name`, creating a default one on `clock` if absent.
  std::shared_ptr<BeamModel> acquire(const std::string& name, const Clock& clock);
  void set(const std::string& name, std::shared_ptr<BeamModel> model);
  void clear();

 private:
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<BeamModel>> models_;
};

}  // namespace pychron::sim
