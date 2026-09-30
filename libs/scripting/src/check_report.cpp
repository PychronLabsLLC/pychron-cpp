#include <algorithm>

#include "pychron/scripting/script_host.hpp"

namespace pychron::scripting {

bool CheckReport::ok() const noexcept {
  return std::none_of(diagnostics.begin(), diagnostics.end(),
                      [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; });
}

std::vector<Diagnostic> CheckReport::errors() const {
  std::vector<Diagnostic> out;
  for (const auto& d : diagnostics)
    if (d.severity == Diagnostic::Severity::Error) out.push_back(d);
  return out;
}

std::vector<Diagnostic> CheckReport::warnings() const {
  std::vector<Diagnostic> out;
  for (const auto& d : diagnostics)
    if (d.severity == Diagnostic::Severity::Warning) out.push_back(d);
  return out;
}

bool CheckReport::has(std::string_view code) const noexcept {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const Diagnostic& d) { return d.code == code; });
}

extraction::CapabilitySet effective_capabilities(const ScriptEnvironment& env) {
  return env.capabilities ? *env.capabilities : extraction::capabilities(env.line);
}

}  // namespace pychron::scripting
