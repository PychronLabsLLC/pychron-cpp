#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::spectrometer {

// How a detector's control points are turned into a mass -> value curve.
// Polynomial fits are least squares in mass (centered for conditioning).
enum class FitKind { Discrete, Linear, Quadratic, Cubic };

// Units of the table values; must match the positioner's native axis.
enum class TableAxis { Dac, Field, Mass };

std::string_view to_string(FitKind kind) noexcept;
std::string_view to_string(TableAxis axis) noexcept;
// Accepts pychron's "parabolic" as an alias for quadratic.
Result<FitKind> parse_fit_kind(std::string_view text);
Result<TableAxis> parse_table_axis(std::string_view text);

// One row of the table: an isotope, its mass, and the native value that puts
// it on each detector. A detector may be absent from a row.
struct ControlPoint {
  std::string isotope;
  double mass = 0.0;
  std::map<std::string, double> values;  // detector -> native value

  friend bool operator==(const ControlPoint&, const ControlPoint&) = default;
};

// Mass <-> native value mapping per detector. Pure: no I/O, no clock.
class FieldTable {
 public:
  // Discrete lookups match a control point within this many amu.
  static constexpr double kDiscreteTolerance = 0.15;
  // mass_for brackets its root over the detector's mass range widened by
  // this many amu on each side.
  static constexpr double kInverseMargin = 1.0;

  FieldTable() = default;
  FieldTable(FitKind fit, TableAxis axis, std::vector<ControlPoint> points);

  FitKind default_fit() const noexcept { return fit_; }
  TableAxis axis() const noexcept { return axis_; }
  const std::vector<ControlPoint>& points() const noexcept { return points_; }
  // Detectors in first-appearance order across points.
  std::vector<std::string> detectors() const;
  bool has_detector(std::string_view det) const;

  // Per-detector fit; falls back to the table default.
  FitKind fit(std::string_view det) const;
  void set_fit(std::string det, FitKind kind);
  const std::map<std::string, FitKind, std::less<>>& fit_overrides() const noexcept { return fits_; }

  // Native value that centers `mass` on `det`. Discrete: nearest control
  // point within kDiscreteTolerance, else Error{Config}. Never falls back to
  // any "current" position.
  Result<double> value_for(double mass, std::string_view det) const;

  // Bracketed numeric inverse of value_for over the detector's mass range.
  Result<double> mass_for(double value, std::string_view det) const;

  // Sets `det`'s value at `isotope`. With `propagate`, the same offset is
  // added to every other detector that has a value at that isotope.
  Result<void> update(std::string_view det, std::string_view isotope, double new_value, bool propagate);

  friend bool operator==(const FieldTable&, const FieldTable&) = default;

 private:
  FitKind fit_ = FitKind::Quadratic;
  TableAxis axis_ = TableAxis::Dac;
  std::vector<ControlPoint> points_;
  std::map<std::string, FitKind, std::less<>> fits_;
};

// TOML form used under tables/<name>/<version>.toml:
//
//   fit = "quadratic"
//   axis = "dac"
//   [fits]            # optional per-detector overrides
//   CDD = "linear"
//   [[points]]
//   isotope = "Ar40"
//   mass = 39.962
//   H1 = 5.001
//
// Every numeric key in a point other than `mass` is a detector value.
Result<FieldTable> parse_field_table(std::string_view toml_text);
std::string to_toml(const FieldTable& table);

}  // namespace pychron::spectrometer
