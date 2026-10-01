#pragma once

// Private bridge between Logger and LogHub::Impl (which is defined, with its
// spdlog members, in log_hub.cpp). Not installed; not part of the public API.

#include <cstdint>
#include <string_view>

#include "pychron/core/log_hub.hpp"

namespace pychron::detail {

// Current rule epoch (>= 1); changes on every LogHub::set_level.
std::uint64_t hub_rule_epoch(const LogHub::Impl& hub) noexcept;

// Level for `name` under the current rules, together with the epoch those
// rules belong to (read under the same lock, so the pair is consistent).
struct ResolvedLevel {
  LogLevel level;
  std::uint64_t epoch;
};
ResolvedLevel hub_resolve_level(const LogHub::Impl& hub, std::string_view name) noexcept;

// LogHub::write on the Impl; a no-op once the hub has been destroyed.
void hub_write(LogHub::Impl& hub, LogLevel level, std::string_view logger,
               std::string_view message) noexcept;

}  // namespace pychron::detail
