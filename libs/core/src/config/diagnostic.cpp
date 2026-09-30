#include "pychron/core/config/diagnostic.hpp"

namespace pychron::config {

std::string to_string(const Diagnostic& d) {
  std::string out = d.loc.file.empty() ? std::string("<unknown>") : d.loc.file;
  out += ':';
  out += std::to_string(d.loc.line);
  out += ':';
  out += d.field;
  out += ": ";
  out += d.message;
  return out;
}

Error to_error(const std::vector<Diagnostic>& diagnostics) {
  std::string what;
  for (const auto& d : diagnostics) {
    if (!what.empty()) what += '\n';
    what += to_string(d);
  }
  return Error{ErrorKind::Config, std::move(what), {}};
}

}  // namespace pychron::config
