#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"

namespace pychron {

enum class HealthState { Connected, Degraded, Down };

std::string_view to_string(HealthState state) noexcept;

// Snapshot of a transport's link quality. Connected: open and the last
// operation succeeded. Degraded: open, but recent operations failed. Down:
// closed, or failures reached the transport's down threshold.
struct Health {
  HealthState state = HealthState::Down;
  TimePoint last_ok{};  // time of the last successful operation (epoch if none)
  std::uint64_t consecutive_failures = 0;
  std::string last_error;

  friend bool operator==(const Health&, const Health&) = default;
};

}  // namespace pychron
