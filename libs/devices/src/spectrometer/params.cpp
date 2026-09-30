#include "pychron/devices/spectrometer/params.hpp"

#include <array>

namespace pychron::spectrometer {
namespace {

constexpr std::string_view kCustomPrefix = "custom:";

constexpr std::array<SourceParamInfo, 19> kRegistry{{
    {SourceParam::HV, "hv", Unit::Volts},
    {SourceParam::TrapCurrent, "trap_current", Unit::MicroAmps},
    {SourceParam::TrapVoltage, "trap_voltage", Unit::Volts},
    {SourceParam::Emission, "emission", Unit::MicroAmps},
    {SourceParam::ElectronEnergy, "electron_energy", Unit::ElectronVolts},
    {SourceParam::IonRepeller, "ion_repeller", Unit::Volts},
    {SourceParam::ExtractionLens, "extraction_lens", Unit::Percent},
    {SourceParam::ExtractionFocus, "extraction_focus", Unit::Percent},
    {SourceParam::ExtractionSymmetry, "extraction_symmetry", Unit::Percent},
    {SourceParam::YSymmetry, "y_symmetry", Unit::Volts},
    {SourceParam::ZSymmetry, "z_symmetry", Unit::Volts},
    {SourceParam::ZFocus, "z_focus", Unit::Volts},
    {SourceParam::HorizontalSymmetry, "horizontal_symmetry", Unit::Volts},
    {SourceParam::Flatapole, "flatapole", Unit::Volts},
    {SourceParam::RotationQuad, "rotation_quad", Unit::Volts},
    {SourceParam::PoleN, "pole_n", Unit::Volts},
    {SourceParam::PoleS, "pole_s", Unit::Volts},
    {SourceParam::ESAPlus, "esa_plus", Unit::Volts},
    {SourceParam::ESAMinus, "esa_minus", Unit::Volts},
}};

constexpr bool registry_in_enum_order() {
  for (std::size_t i = 0; i < kRegistry.size(); ++i) {
    if (static_cast<std::size_t>(kRegistry[i].param) != i) return false;
  }
  return true;
}
static_assert(registry_in_enum_order(), "kRegistry must list SourceParam in enum order");
static_assert(static_cast<std::size_t>(SourceParam::ESAMinus) + 1 == kRegistry.size(),
              "kRegistry must cover every SourceParam");

}  // namespace

std::span<const SourceParamInfo> source_params() noexcept { return kRegistry; }

const SourceParamInfo& info(SourceParam param) noexcept {
  return kRegistry[static_cast<std::size_t>(param)];
}

std::string_view to_string(SourceParam param) noexcept { return info(param).name; }

std::string to_string(const ParamId& id) {
  if (const auto* p = std::get_if<SourceParam>(&id)) return std::string(to_string(*p));
  return std::string(kCustomPrefix) + std::get<Custom>(id).name;
}

Result<ParamId> parse_param_id(std::string_view text) {
  if (text.starts_with(kCustomPrefix)) {
    text.remove_prefix(kCustomPrefix.size());
    if (text.empty()) return fail(ErrorKind::Config, "custom parameter has no name");
    return ParamId{Custom{std::string(text)}};
  }
  for (const auto& entry : kRegistry) {
    if (entry.name == text) return ParamId{entry.param};
  }
  return fail(ErrorKind::Config, "unknown source parameter '" + std::string(text) + "'");
}

std::string_view to_string(Unit unit) noexcept {
  switch (unit) {
    case Unit::None: return "";
    case Unit::Volts: return "V";
    case Unit::MicroAmps: return "uA";
    case Unit::ElectronVolts: return "eV";
    case Unit::Percent: return "%";
  }
  return "";
}

const ParamSpec* find_spec(std::span<const ParamSpec> specs, const ParamId& id) noexcept {
  for (const auto& spec : specs) {
    if (spec.id == id) return &spec;
  }
  return nullptr;
}

const ParamSpec* find_vendor(std::span<const ParamSpec> specs,
                             std::string_view vendor_name) noexcept {
  for (const auto& spec : specs) {
    if (spec.vendor_name == vendor_name) return &spec;
  }
  return nullptr;
}

}  // namespace pychron::spectrometer
