#include "trace_settings.hpp"

#include <fstream>

namespace elctl {

namespace fs = std::filesystem;

fs::path trace_state_path(const fs::path& config_file) { return config_file.parent_path() / ".elctl-trace"; }

fs::path trace_dir(const fs::path& config_file) { return config_file.parent_path() / "traces"; }

TraceSettings load_trace_settings(const fs::path& config_file) {
  TraceSettings settings;
  std::ifstream in(trace_state_path(config_file));
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    if (line == "*") {
      settings.all = true;
    } else {
      settings.transports.insert(line);
    }
  }
  return settings;
}

pychron::Result<void> save_trace_settings(const fs::path& config_file, const TraceSettings& settings) {
  const fs::path path = trace_state_path(config_file);
  if (!settings.enabled()) {
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) return pychron::fail(pychron::ErrorKind::Io, "cannot remove " + path.string() + ": " + ec.message());
    return {};
  }
  std::ofstream out(path, std::ios::trunc);
  if (settings.all) out << "*\n";
  for (const auto& t : settings.transports) out << t << '\n';
  if (!out) return pychron::fail(pychron::ErrorKind::Io, "cannot write " + path.string());
  return {};
}

}  // namespace elctl
