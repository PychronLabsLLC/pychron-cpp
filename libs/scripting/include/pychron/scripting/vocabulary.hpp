#pragma once

// The pyscript vocabulary: every command a script may call, the capability
// it needs and the script kinds that may use it. The Python signatures (and
// so arity) live with the bindings; a test keeps both lists identical.

#include <optional>
#include <span>
#include <string_view>

#include "pychron/devices/extraction/capability.hpp"
#include "pychron/scripting/script.hpp"

namespace pychron::scripting {

struct CommandInfo {
  std::string_view name;
  // Needed device/line feature; nullopt for commands that need none (or
  // only the run's extraction device, checked when called).
  std::optional<extraction::Capability> capability;
  // Blocking commands check the CancelToken and raise ScriptCancelled.
  bool blocking = false;
  // Its first argument is a valve name the static check resolves.
  bool valve_argument = false;
  // Only PostMeasurement scripts may call it.
  bool post_measurement_only = false;
};

std::span<const CommandInfo> vocabulary() noexcept;
const CommandInfo* find_command(std::string_view name) noexcept;
bool command_allowed(const CommandInfo& command, ScriptKind kind) noexcept;

// Modules scripts may import.
std::span<const std::string_view> default_import_allowlist() noexcept;

}  // namespace pychron::scripting
