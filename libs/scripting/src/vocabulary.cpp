#include "pychron/scripting/vocabulary.hpp"

#include <algorithm>
#include <array>

namespace pychron::scripting {
namespace {

using extraction::Capability;

constexpr auto kNone = std::optional<Capability>{};

// name, capability, blocking, valve_argument, post_measurement_only
constexpr std::array kVocabulary = {
    // valves
    CommandInfo{"open", Capability::Valves, false, true, false},
    CommandInfo{"close", Capability::Valves, false, true, false},
    CommandInfo{"lock", Capability::Valves, false, true, false},
    CommandInfo{"unlock", Capability::Valves, false, true, false},
    CommandInfo{"is_open", Capability::Valves, false, true, false},
    CommandInfo{"is_closed", Capability::Valves, false, true, false},
    // extraction device
    CommandInfo{"extract", kNone, false, false, false},
    CommandInfo{"end_extract", kNone, false, false, false},
    CommandInfo{"ramp", kNone, true, false, false},
    CommandInfo{"enable", kNone, false, false, false},
    CommandInfo{"disable", kNone, false, false, false},
    CommandInfo{"prepare", kNone, false, false, false},
    CommandInfo{"get_device", kNone, false, false, false},
    CommandInfo{"fire_laser", Capability::Laser, false, false, false},
    CommandInfo{"warmup", Capability::Laser, false, false, false},
    // stage / patterns
    CommandInfo{"move_to_position", Capability::Stage, true, false, false},
    CommandInfo{"set_x", Capability::Stage, true, false, false},
    CommandInfo{"set_y", Capability::Stage, true, false, false},
    CommandInfo{"set_z", Capability::Stage, true, false, false},
    CommandInfo{"set_xy", Capability::Stage, true, false, false},
    CommandInfo{"set_tray", Capability::Stage, false, false, false},
    CommandInfo{"execute_pattern", Capability::Pattern, true, false, false},
    // furnace
    CommandInfo{"dump_sample", Capability::Furnace, false, false, false},
    CommandInfo{"drop_sample", Capability::Furnace, false, false, false},
    CommandInfo{"set_pid_parameters", Capability::Furnace, false, false, false},
    CommandInfo{"begin_heating_interval", Capability::Furnace, false, false, false},
    // pipettes, motors, cryo
    CommandInfo{"load_pipette", Capability::Pipette, false, false, false},
    CommandInfo{"extract_pipette", Capability::Pipette, false, false, false},
    CommandInfo{"set_motor", Capability::Motor, true, false, false},
    CommandInfo{"get_value", Capability::Motor, false, false, false},
    CommandInfo{"set_cryo", Capability::Cryo, false, false, false},
    CommandInfo{"get_cryo_temp", Capability::Cryo, false, false, false},
    // imaging
    CommandInfo{"snapshot", Capability::Imaging, false, false, false},
    CommandInfo{"video_start", Capability::Imaging, false, false, false},
    CommandInfo{"video_stop", Capability::Imaging, false, false, false},
    CommandInfo{"video_recording", Capability::Imaging, false, false, false},
    // pressure
    CommandInfo{"get_pressure", Capability::Pressure, false, false, false},
    CommandInfo{"get_manometer_pressure", Capability::Pressure, false, false, false},
    // timing
    CommandInfo{"waitfor", kNone, true, false, false},
    CommandInfo{"wake", kNone, false, false, false},
    CommandInfo{"pause", kNone, true, false, false},
    CommandInfo{"sleep", kNone, true, false, false},
    CommandInfo{"delay", kNone, true, false, false},
    CommandInfo{"begin_interval", kNone, false, false, false},
    CommandInfo{"complete_interval", kNone, true, false, false},
    // shared resources
    CommandInfo{"acquire", kNone, true, false, false},
    CommandInfo{"wait", kNone, true, false, false},
    CommandInfo{"release", kNone, false, false, false},
    CommandInfo{"set_resource", kNone, false, false, false},
    CommandInfo{"get_resource_value", kNone, false, false, false},
    // misc
    CommandInfo{"info", kNone, false, false, false},
    CommandInfo{"gosub", kNone, false, false, false},
    // post_measurement
    CommandInfo{"get_intensity", kNone, false, false, true},
    CommandInfo{"signal_pump_time_start", kNone, false, false, true},
};

constexpr std::array<std::string_view, 11> kImports = {
    "math", "random", "statistics", "json", "re", "datetime",
    "collections", "itertools", "functools", "string", "operator"};

}  // namespace

std::span<const CommandInfo> vocabulary() noexcept { return kVocabulary; }

const CommandInfo* find_command(std::string_view name) noexcept {
  auto it = std::find_if(kVocabulary.begin(), kVocabulary.end(),
                         [&](const CommandInfo& c) { return c.name == name; });
  return it == kVocabulary.end() ? nullptr : &*it;
}

bool command_allowed(const CommandInfo& command, ScriptKind kind) noexcept {
  return !command.post_measurement_only || kind == ScriptKind::PostMeasurement;
}

std::span<const std::string_view> default_import_allowlist() noexcept { return kImports; }

}  // namespace pychron::scripting
