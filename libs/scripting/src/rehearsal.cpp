// What a rehearsal refuses: an environment with a live service in it.

#include "pychron/scripting/script_host.hpp"

namespace pychron::scripting {

std::optional<std::string> rehearsal_refusal(const ScriptEnvironment& env) {
  if (env.line.device) return "line.device";
  if (env.line.valves) return "line.valves";
  if (env.line.pressure) return "line.pressure";
  if (env.line.cryo) return "line.cryo";
  if (env.intensity) return "intensity";
  if (env.measurement) return "measurement";
  if (env.resources) return "resources";
  if (env.on_pump_time_start) return "on_pump_time_start";
  return std::nullopt;
}

}  // namespace pychron::scripting
