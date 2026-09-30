#pragma once

// Canonical source-parameter registry. Every IBeamSource advertises the subset
// it supports as ParamSpecs carrying its own vendor names (Qtegra
// "Y-Symmetry Set", NGX "YF"); a legacy supply exposes only HV. Vendor extras
// outside the registry use ParamId{Custom{name}}.

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::spectrometer {

enum class SourceParam {
  HV,
  TrapCurrent,
  TrapVoltage,
  Emission,
  ElectronEnergy,
  IonRepeller,
  ExtractionLens,
  ExtractionFocus,
  ExtractionSymmetry,
  YSymmetry,
  ZSymmetry,
  ZFocus,
  HorizontalSymmetry,
  Flatapole,
  RotationQuad,
  PoleN,
  PoleS,
  ESAPlus,
  ESAMinus,
};

// A vendor parameter outside the canonical registry, identified by name.
struct Custom {
  std::string name;

  friend bool operator==(const Custom&, const Custom&) = default;
  friend auto operator<=>(const Custom&, const Custom&) = default;
};

using ParamId = std::variant<SourceParam, Custom>;

enum class Unit { None, Volts, MicroAmps, ElectronVolts, Percent };

struct ParamSpec {
  ParamId id;
  Unit unit = Unit::None;
  Range range;
  bool readable = true;
  bool writable = true;
  std::string vendor_name;
};

// Registry entry: canonical config name and default unit.
struct SourceParamInfo {
  SourceParam param;
  std::string_view name;  // snake_case, used in profiles/config: "trap_current"
  Unit unit;
};

// Every canonical SourceParam, in enum order.
std::span<const SourceParamInfo> source_params() noexcept;

const SourceParamInfo& info(SourceParam param) noexcept;

// Canonical name, e.g. "y_symmetry".
std::string_view to_string(SourceParam param) noexcept;

// Canonical names, or "custom:<name>" for Custom.
std::string to_string(const ParamId& id);

// Inverse of to_string(ParamId); Config error for unknown canonical names.
Result<ParamId> parse_param_id(std::string_view text);

// "V", "uA", "eV", "%", "".
std::string_view to_string(Unit unit) noexcept;

// The spec for `id` in `specs`, or nullptr.
const ParamSpec* find_spec(std::span<const ParamSpec> specs, const ParamId& id) noexcept;

// The spec whose vendor_name is `vendor_name`, or nullptr.
const ParamSpec* find_vendor(std::span<const ParamSpec> specs, std::string_view vendor_name) noexcept;

}  // namespace pychron::spectrometer
