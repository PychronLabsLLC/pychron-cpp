#pragma once

// The lab's scripts for editing (experiment-window design, script editor):
// where they live, what a script may call, its static check and estimate
// without hardware, and its gosubs.
//
// Scripts live at <lab>/scripts/<kind>/<name>.py; a queue names them without
// ".py", with ':' for subdirectories ("co2:degas" is co2/degas.py). gosub()
// targets resolve in the calling script's kind directory, then lib/.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/scripting/script_host.hpp"

namespace pychron::experiment::lab {

struct ScriptFile {
  scripting::ScriptKind kind = scripting::ScriptKind::Extraction;
  std::string name;              // queue name: "sim_extract", "co2:degas"
  std::filesystem::path path;    // absolute
  friend bool operator==(const ScriptFile&, const ScriptFile&) = default;
};

// Every script of every kind (kind order, then name), and the lib/ directory
// gosubs fall back to (kind Extraction, name "lib:<name>").
std::vector<ScriptFile> lab_scripts(const Lab& lab);

// <lab>/scripts/<kind>/<name>.py; Config error for an unsafe name.
Result<std::filesystem::path> script_path(const Lab& lab, scripting::ScriptKind kind, std::string_view name);

// What check() and estimate() see without hardware: every capability, the
// extraction line's valve names, the default run context, the lab's script
// resolver (gosubs) and a log sink that drops info().
scripting::ScriptEnvironment editor_environment(const Lab& lab);

struct ScriptCheck {
  scripting::CheckReport report;
  std::optional<scripting::Estimate> estimate;  // when the check passed
  std::string error;                            // the host could not check (no scripting) or estimate failed
};

// Static check, then (if it passed) the estimate. `name` is the queue name,
// used in diagnostics.
ScriptCheck check_script(scripting::IScriptHost& host, const Lab& lab, scripting::ScriptKind kind,
                         std::string_view name, std::string_view text);

// Words to complete in a script of `kind`: the commands it may call, the
// run-context globals, `opt`, the allowed builtins and Python keywords.
// Sorted, unique.
std::vector<std::string> completion_words(scripting::ScriptKind kind);

// Python keywords and the allowed builtins, for highlighting.
const std::vector<std::string>& python_keywords();
const std::vector<std::string>& script_builtins();

struct GosubRef {
  int line = 0;        // 1-based
  int column = 0;      // 0-based offset of the name inside the quotes
  int length = 0;
  std::string name;
  friend bool operator==(const GosubRef&, const GosubRef&) = default;
};

// gosub('name') / gosub("name", ...) calls with a literal name, in order.
// Commented-out calls are skipped.
std::vector<GosubRef> find_gosubs(std::string_view text);

// The file a gosub from a script of `from` runs, as the host resolves it:
// <kind>/<name>.py, then lib/<name>.py. nullopt when neither exists.
std::optional<ScriptFile> resolve_gosub(const Lab& lab, scripting::ScriptKind from, std::string_view name);

}  // namespace pychron::experiment::lab
