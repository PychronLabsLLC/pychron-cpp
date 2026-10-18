#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"

namespace pychron::spectrometer {

// Checks of a field table before it is saved or made current
// (docs/superpowers/specs/2026-10-18-field-table-editor-design.md, 3.2).
// Errors block a save; warnings do not.
enum class TableSeverity { Error, Warning };

struct TableDiagnostic {
  TableSeverity severity = TableSeverity::Error;
  std::optional<std::size_t> point;  // index into FieldTable::points()
  std::string detector;              // empty: row-level or table-level
  std::string message;

  friend bool operator==(const TableDiagnostic&, const TableDiagnostic&) = default;
};

using TableDiagnostics = std::vector<TableDiagnostic>;

[[nodiscard]] bool has_error(const TableDiagnostics& d) noexcept;
// First diagnostic of severity Error, or nullptr.
[[nodiscard]] const TableDiagnostic* first_error(const TableDiagnostics& d) noexcept;

// Thresholds of the warnings.
inline constexpr int kMonotonicSamples = 200;
inline constexpr double kResidualSpanFraction = 0.01;
inline constexpr double kWeightTolerance = 0.01;  // amu

// Every error and warning of `table`, stored under `name`. The active
// detector rule applies only when `name` is config's `[magnet] field_table`
// or `hv_table`. Isotope names are compared trimmed of surrounding blanks.
// Pure: no I/O.
[[nodiscard]] TableDiagnostics check_field_table(const FieldTable& table, const cfg::SpectrometerConfig& config,
                                                 const MolecularWeights& weights, std::string_view name);

// "<severity>: [row <n>][/<detector>]: <message>", row 1-based; the location
// is left out when the diagnostic has neither, e.g. "error: row 3/H1: value
// is not finite", "warning: H2: detector is not in config".
std::string to_string(const TableDiagnostic& d);

}  // namespace pychron::spectrometer
