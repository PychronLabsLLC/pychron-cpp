#include "pychron/experiment/lab/scripts.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "pychron/scripting/vocabulary.hpp"

namespace pychron::experiment::lab {

namespace fs = std::filesystem;
using scripting::ScriptKind;

namespace {

constexpr ScriptKind kKinds[] = {ScriptKind::Extraction, ScriptKind::PostEquilibration, ScriptKind::PostMeasurement,
                                 ScriptKind::MeasurementHook};

fs::path scripts_root(const Lab& lab) { return lab.paths.dir / "scripts"; }

// Queue name of `file` relative to `dir`: "co2/degas.py" -> "co2:degas".
std::string queue_name(const fs::path& dir, const fs::path& file) {
  std::error_code ec;
  fs::path rel = fs::relative(file, dir, ec);
  rel.replace_extension();
  std::string name;
  for (const auto& part : rel) name += (name.empty() ? "" : ":") + part.string();
  return name;
}

std::vector<fs::path> python_files(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec))
    if (it->is_regular_file(ec) && it->path().extension() == ".py") out.push_back(it->path());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::vector<ScriptFile> lab_scripts(const Lab& lab) {
  std::vector<ScriptFile> out;
  for (auto kind : kKinds) {
    const fs::path dir = scripts_root(lab) / std::string(scripting::to_string(kind));
    for (const auto& f : python_files(dir)) out.push_back({kind, queue_name(dir, f), fs::absolute(f)});
  }
  const fs::path lib = scripts_root(lab) / "lib";
  for (const auto& f : python_files(lib)) out.push_back({ScriptKind::Extraction, "lib:" + queue_name(lib, f), fs::absolute(f)});
  return out;
}

Result<fs::path> script_path(const Lab& lab, ScriptKind kind, std::string_view name) {
  auto rel = scripting::normalize_script_name(name);
  if (!rel) return fail(rel.error());
  return fs::absolute(scripts_root(lab) / std::string(scripting::to_string(kind)) / *rel);
}

scripting::ScriptEnvironment editor_environment(const Lab& lab) {
  scripting::ScriptEnvironment env;
  extraction::CapabilitySet all;
  for (std::size_t i = 0; i < extraction::kCapabilityCount; ++i) all.add(static_cast<extraction::Capability>(i));
  env.capabilities = all;
  std::vector<std::string> valves;
  if (lab.line)
    for (const auto& v : lab.line->valves) valves.push_back(v.name);
  env.valve_names = std::move(valves);
  env.resolver = lab.scripts ? &lab.scripts->resolver() : nullptr;
  env.context = scripting::make_context();
  env.log = [](std::string_view) {};
  return env;
}

ScriptCheck check_script(scripting::IScriptHost& host, const Lab& lab, ScriptKind kind, std::string_view name,
                         std::string_view text) {
  ScriptCheck out;
  const scripting::Script script{std::string(name), std::string(text), kind};
  const auto env = editor_environment(lab);
  auto report = host.check(script, env);
  if (!report) {
    out.error = report.error().what;
    return out;
  }
  out.report = std::move(*report);
  if (!out.report.ok()) return out;
  auto estimate = host.estimate(script, env);
  if (estimate) out.estimate = std::move(*estimate);
  else out.error = estimate.error().what;
  return out;
}

const std::vector<std::string>& python_keywords() {
  static const std::vector<std::string> words{
      "False", "None",   "True",  "and",    "as",     "assert", "break",  "class", "continue",
      "def",   "del",    "elif",  "else",   "except", "finally", "for",   "from",  "global",
      "if",    "import", "in",    "is",     "lambda", "nonlocal", "not",  "or",    "pass",
      "raise", "return", "try",   "while",  "with",   "yield"};
  return words;
}

const std::vector<std::string>& script_builtins() {
  // The host's SAFE_BUILTINS that a script would type (python/sources.cpp).
  static const std::vector<std::string> words{
      "abs",   "all",   "any",    "bool",      "callable", "chr",   "dict",     "divmod",  "enumerate", "filter",
      "float", "format", "frozenset", "hasattr", "hash",   "int",   "isinstance", "issubclass", "iter", "len",
      "list",  "map",   "max",    "min",       "next",     "ord",   "pow",      "range",   "repr",      "reversed",
      "round", "set",   "slice",  "sorted",    "str",      "sum",   "tuple",    "zip",     "Exception", "RuntimeError",
      "TypeError", "ValueError", "KeyError", "IndexError"};
  return words;
}

std::vector<std::string> completion_words(ScriptKind kind) {
  std::set<std::string> out;
  for (const auto& c : scripting::vocabulary())
    if (scripting::command_allowed(c, kind)) out.insert(std::string(c.name));
  for (const auto& [name, value] : scripting::default_context()) out.insert(name);
  out.insert("opt");
  for (const auto& w : script_builtins()) out.insert(w);
  for (const auto& w : python_keywords()) out.insert(w);
  return {out.begin(), out.end()};
}

std::vector<GosubRef> find_gosubs(std::string_view text) {
  std::vector<GosubRef> out;
  int line = 1;
  std::size_t start = 0;
  while (start <= text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view s = text.substr(start, end - start);
    // One pass over the line: quotes and comments are skipped, so only calls
    // in code count (single-line strings are close enough for scripts).
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
      const char ch = s[i];
      if (quote != 0) {
        if (ch == '\\') ++i;
        else if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '#') break;
      if (ch == '\'' || ch == '"') {
        quote = ch;
        continue;
      }
      if (s.compare(i, 5, "gosub") != 0) continue;
      if (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_')) continue;
      std::size_t j = i + 5;
      while (j < s.size() && s[j] == ' ') ++j;
      if (j >= s.size() || s[j] != '(') continue;
      ++j;
      while (j < s.size() && s[j] == ' ') ++j;
      if (j >= s.size() || (s[j] != '\'' && s[j] != '"')) continue;
      const std::size_t name_start = j + 1;
      const std::size_t name_end = s.find(s[j], name_start);
      if (name_end == std::string_view::npos) break;
      if (name_end > name_start)
        out.push_back({line, static_cast<int>(name_start), static_cast<int>(name_end - name_start),
                       std::string(s.substr(name_start, name_end - name_start))});
      i = name_end;  // past the closing quote
    }
    ++line;
    if (end == text.size()) break;
    start = end + 1;
  }
  return out;
}

std::optional<ScriptFile> resolve_gosub(const Lab& lab, ScriptKind from, std::string_view name) {
  auto rel = scripting::normalize_script_name(name);
  if (!rel) return std::nullopt;
  const fs::path root = scripts_root(lab);
  std::error_code ec;
  const fs::path in_kind = root / std::string(scripting::to_string(from)) / *rel;
  if (fs::is_regular_file(in_kind, ec)) return ScriptFile{from, std::string(name), fs::absolute(in_kind)};
  const fs::path in_lib = root / "lib" / *rel;
  if (fs::is_regular_file(in_lib, ec)) return ScriptFile{ScriptKind::Extraction, "lib:" + std::string(name), fs::absolute(in_lib)};
  return std::nullopt;
}

}  // namespace pychron::experiment::lab
