#include "pychron/devices/extraction/capability.hpp"

#include <array>

#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/devices/extraction/services.hpp"

namespace pychron::extraction {
namespace {

constexpr std::array<std::string_view, kCapabilityCount> kNames{
    "laser", "furnace", "stage", "pattern", "pipette",
    "cryo",  "motor",   "imaging", "pressure", "valves",
};

constexpr std::string_view kNotSupported = "not supported: ";

}  // namespace

std::string_view to_string(Capability capability) noexcept {
  auto i = static_cast<std::size_t>(capability);
  return i < kNames.size() ? kNames[i] : std::string_view{"unknown"};
}

std::optional<Capability> capability_from_string(std::string_view name) noexcept {
  for (std::size_t i = 0; i < kNames.size(); ++i)
    if (kNames[i] == name) return static_cast<Capability>(i);
  return std::nullopt;
}

std::vector<Capability> CapabilitySet::list() const {
  std::vector<Capability> out;
  for (std::size_t i = 0; i < kCapabilityCount; ++i)
    if (bits_.test(i)) out.push_back(static_cast<Capability>(i));
  return out;
}

Error not_supported(std::string_view what, std::string device) {
  return Error{ErrorKind::Config, std::string(kNotSupported) + std::string(what), std::move(device)};
}

Error not_supported(Capability capability, std::string device) {
  return not_supported(to_string(capability), std::move(device));
}

bool is_not_supported(const Error& error) noexcept {
  return error.kind == ErrorKind::Config && error.what.starts_with(kNotSupported);
}

std::string_view to_string(ExtractUnits units) noexcept {
  switch (units) {
    case ExtractUnits::Percent: return "percent";
    case ExtractUnits::Watts: return "watts";
    case ExtractUnits::Celsius: return "temp";
  }
  return "unknown";
}

std::optional<ExtractUnits> extract_units_from_string(std::string_view name) noexcept {
  for (auto u : {ExtractUnits::Percent, ExtractUnits::Watts, ExtractUnits::Celsius})
    if (to_string(u) == name) return u;
  return std::nullopt;
}

CapabilitySet capabilities(IExtractionDevice& device) {
  CapabilitySet set;
  if (device.laser()) set.add(Capability::Laser);
  if (device.furnace()) set.add(Capability::Furnace);
  if (device.stage()) set.add(Capability::Stage);
  if (device.pattern_runner()) set.add(Capability::Pattern);
  if (device.pipettes()) set.add(Capability::Pipette);
  if (device.cryo()) set.add(Capability::Cryo);
  if (device.motors()) set.add(Capability::Motor);
  if (device.imaging()) set.add(Capability::Imaging);
  return set;
}

CapabilitySet capabilities(const ExtractionServices& services) {
  CapabilitySet set = services.device ? capabilities(*services.device) : CapabilitySet{};
  if (services.valves) set.add(Capability::Valves);
  if (services.pressure) set.add(Capability::Pressure);
  return set;
}

}  // namespace pychron::extraction
