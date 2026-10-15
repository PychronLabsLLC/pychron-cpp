#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "pychron/scripting/services.hpp"

namespace pychron::scripting {

Result<std::string> normalize_script_name(std::string_view name) {
  std::string out(name);
  for (auto& c : out)
    if (c == ':') c = '/';
  if (out.empty() || out.front() == '/' || out.find('\\') != std::string::npos)
    return fail(ErrorKind::Config, "bad script name '" + std::string(name) + "'");
  std::filesystem::path p(out);
  for (const auto& part : p)
    if (part == "..") return fail(ErrorKind::Config, "bad script name '" + std::string(name) + "'");
  if (!out.ends_with(".py")) out += ".py";
  return out;
}

namespace {

Unexpected<Error> not_found(std::string_view name) {
  return fail(Error{ErrorKind::Config, "gosub '" + std::string(name) + "' not found", {}, kScriptNotFound});
}

}  // namespace

Result<Script> DirectoryScriptResolver::resolve(std::string_view name, ScriptKind from) const {
  const std::string kind_dir(to_string(from));
  // The lab's safe state is one file, named by the kind itself (or by no
  // name); any other name is a gosub from it and is looked for under lib/.
  const bool safe_state = from == ScriptKind::SafeState;
  if (safe_state && (name.empty() || name == kind_dir || name == kind_dir + ".py")) {
    const std::string file = kind_dir + ".py";
    std::error_code ec;
    if (!std::filesystem::exists(root_ / file, ec)) return not_found(file);
    std::ifstream in(root_ / file, std::ios::binary);
    std::ostringstream text;
    if (in) text << in.rdbuf();
    if (!in || std::filesystem::is_directory(root_ / file, ec))
      return fail(ErrorKind::Io, "cannot read " + (root_ / file).string());
    return Script{file, text.str(), from};
  }
  auto rel = normalize_script_name(name);
  if (!rel) return fail(rel.error());
  for (const auto& dir : safe_state ? std::vector<std::string>{"lib"} : std::vector<std::string>{kind_dir, "lib"}) {
    auto path = root_ / dir / *rel;
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::ostringstream text;
    text << in.rdbuf();
    return Script{dir + "/" + *rel, text.str(), from};
  }
  return not_found(name);
}

Result<Script> MapScriptResolver::resolve(std::string_view name, ScriptKind from) const {
  auto rel = normalize_script_name(name);
  if (!rel) return fail(rel.error());
  for (const auto& key : {std::string(name), *rel}) {
    auto it = scripts_.find(key);
    if (it != scripts_.end()) return Script{*rel, it->second, from};
  }
  return not_found(name);
}

}  // namespace pychron::scripting
