#include "pychron/systems/spectrometer/config.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace pychron::spectrometer::cfg {
namespace {

constexpr std::array kRoles{
    std::pair<std::string_view, Role>{"positioner", Role::Positioner},
    std::pair<std::string_view, Role>{"source", Role::Source},
    std::pair<std::string_view, Role>{"acquirer", Role::Acquirer},
    std::pair<std::string_view, Role>{"detector_control", Role::DetectorControl},
    std::pair<std::string_view, Role>{"beam_blank", Role::BeamBlank},
};

constexpr std::array kAxes{
    std::pair<std::string_view, Axis>{"dac", Axis::Dac},
    std::pair<std::string_view, Axis>{"field", Axis::Field},
    std::pair<std::string_view, Axis>{"mass", Axis::Mass},
};

template <class E, std::size_t N>
std::string_view name_of(const std::array<std::pair<std::string_view, E>, N>& table, E value) noexcept {
  for (const auto& [name, v] : table) {
    if (v == value) return name;
  }
  return "unknown";
}

template <class E, std::size_t N>
std::optional<E> value_of(const std::array<std::pair<std::string_view, E>, N>& table, std::string_view name) noexcept {
  for (const auto& [n, v] : table) {
    if (n == name) return v;
  }
  return std::nullopt;
}

}  // namespace

std::string_view to_string(Role role) noexcept { return name_of(kRoles, role); }
std::optional<Role> role_from_string(std::string_view name) noexcept { return value_of(kRoles, name); }

std::string_view to_string(Axis axis) noexcept { return name_of(kAxes, axis); }
std::optional<Axis> axis_from_string(std::string_view name) noexcept { return value_of(kAxes, name); }

std::string_view to_string(DetectorKind kind) noexcept {
  switch (kind) {
    case DetectorKind::Faraday: return "faraday";
    case DetectorKind::Counter: return "counter";
    case DetectorKind::Cdd: return "cdd";
    case DetectorKind::Atona: return "atona";
  }
  return "unknown";
}

bool DriverConfig::has_role(Role r) const { return std::find(roles.begin(), roles.end(), r) != roles.end(); }

const DriverConfig* SpectrometerConfig::driver(const std::string& name) const {
  auto it = drivers.find(name);
  return it == drivers.end() ? nullptr : &it->second;
}

const DetectorConfig* SpectrometerConfig::detector(const std::string& name) const {
  for (const auto& d : detectors) {
    if (d.name == name) return &d;
  }
  return nullptr;
}

std::optional<ChannelRef> parse_channel_ref(std::string_view text) {
  const auto colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) return std::nullopt;
  return ChannelRef{std::string(text.substr(0, colon)), std::string(text.substr(colon + 1))};
}

}  // namespace pychron::spectrometer::cfg
