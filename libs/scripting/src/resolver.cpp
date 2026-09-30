#include <fstream>
#include <sstream>

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

Result<Script> DirectoryScriptResolver::resolve(std::string_view name, ScriptKind from) const {
  auto rel = normalize_script_name(name);
  if (!rel) return fail(rel.error());
  for (auto dir : {std::string(to_string(from)), std::string("lib")}) {
    auto path = root_ / dir / *rel;
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::ostringstream text;
    text << in.rdbuf();
    return Script{dir + "/" + *rel, text.str(), from};
  }
  return fail(ErrorKind::Config, "gosub '" + std::string(name) + "' not found");
}

Result<Script> MapScriptResolver::resolve(std::string_view name, ScriptKind from) const {
  auto rel = normalize_script_name(name);
  if (!rel) return fail(rel.error());
  for (const auto& key : {std::string(name), *rel}) {
    auto it = scripts_.find(key);
    if (it != scripts_.end()) return Script{*rel, it->second, from};
  }
  return fail(ErrorKind::Config, "gosub '" + std::string(name) + "' not found");
}

}  // namespace pychron::scripting
