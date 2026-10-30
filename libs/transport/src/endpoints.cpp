#include "pychron/transport/endpoints.hpp"

#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>

namespace pychron {

namespace fs = std::filesystem;

namespace {

Unexpected<Error> refused(const fs::path& file, const std::string& why) {
  return fail(ErrorKind::Config, "sim endpoints " + file.generic_string() + ": " + why);
}

const char* protocol_name(WireProtocol p) { return p == WireProtocol::Udp ? "udp" : "tcp"; }

// The canonical form of a path that exists; else its absolute, normal form.
fs::path canonical_or_absolute(const fs::path& p) {
  std::error_code ec;
  auto c = fs::weakly_canonical(p, ec);
  if (!ec) return c;
  return fs::absolute(p, ec).lexically_normal();
}

// The endpoints under `key` (`transports` or `spectrometer_transports`), each
// on `host`.
Result<std::map<std::string, Endpoint>> read_table(const fs::path& file, const toml::table& t, const std::string& key,
                                                   const std::string& host) {
  std::map<std::string, Endpoint> out;
  const auto* transports = t[key].as_table();
  if (transports == nullptr) return out;
  for (const auto& [name_key, node] : *transports) {
    const std::string name = key + "." + std::string(name_key.str());
    const auto* entry = node.as_table();
    if (entry == nullptr) return refused(file, name + " is not a table");
    Endpoint e;
    e.host = host;
    const auto protocol = (*entry)["protocol"].value<std::string>().value_or("");
    if (protocol == "tcp") {
      e.protocol = WireProtocol::Tcp;
    } else if (protocol == "udp") {
      e.protocol = WireProtocol::Udp;
    } else {
      std::string why = name;
      why += ": protocol '";
      why += protocol;
      why += "' is not tcp or udp";
      return refused(file, why);
    }
    const auto port = (*entry)["port"].value<std::int64_t>();
    if (!port || *port < 1 || *port > 65535) return refused(file, name + ": port outside 1..65535");
    e.port = static_cast<std::uint16_t>(*port);
    e.emulator = (*entry)["emulator"].value_or(std::string{});
    out.emplace(std::string(name_key.str()), std::move(e));
  }
  return out;
}

toml::table write_table(const std::map<std::string, Endpoint>& endpoints) {
  toml::table transports;
  for (const auto& [name, e] : endpoints) {
    toml::table entry;
    entry.insert("protocol", protocol_name(e.protocol));
    entry.insert("port", static_cast<std::int64_t>(e.port));
    entry.insert("emulator", e.emulator);
    transports.insert(name, std::move(entry));
  }
  return transports;
}

}  // namespace

Result<SimEndpoints> read_sim_endpoints(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return refused(file, "cannot be read (is sim serve running?)");
  std::ostringstream text;
  text << in.rdbuf();
  toml::parse_result parsed = toml::parse(text.str(), file.generic_string());
  if (!parsed) return refused(file, "is not valid TOML: " + std::string(parsed.error().description()));
  const toml::table& t = parsed.table();

  SimEndpoints out;
  const auto config = t["config"].value<std::string>();
  if (!config || config->empty()) return refused(file, "has no `config`");
  out.config = *config;
  const auto host = t["host"].value<std::string>();
  if (!host || host->empty()) return refused(file, "has no `host`");
  out.host = *host;

  auto line = read_table(file, t, "transports", out.host);
  if (!line) return fail(line.error());
  out.transports = std::move(*line);
  if (const auto spectrometer = t["spectrometer"].value<std::string>(); spectrometer && !spectrometer->empty()) {
    out.spectrometer = *spectrometer;
    auto served = read_table(file, t, "spectrometer_transports", out.host);
    if (!served) return fail(served.error());
    out.spectrometer_transports = std::move(*served);
  }
  return out;
}

Result<void> write_sim_endpoints(const fs::path& file, const SimEndpoints& endpoints) {
  toml::table t;
  t.insert("config", endpoints.config.generic_string());
  t.insert("host", endpoints.host);
  t.insert("transports", write_table(endpoints.transports));
  if (!endpoints.spectrometer.empty()) {
    t.insert("spectrometer", endpoints.spectrometer.generic_string());
    t.insert("spectrometer_transports", write_table(endpoints.spectrometer_transports));
  }

  std::ostringstream text;
  text << "# Written by elctl sim serve while it runs; removed when it stops. Not edited by hand.\n" << t << '\n';

  fs::path temp = file;
  temp += ".writing";
  std::error_code ec;
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return fail(ErrorKind::Io, "cannot write " + temp.generic_string());
    out << text.str();
    out.flush();
    if (!out) {
      out.close();
      fs::remove(temp, ec);
      return fail(ErrorKind::Io, "could not write " + file.generic_string() + " in full");
    }
  }
  fs::rename(temp, file, ec);
  if (ec) {
    const std::string message = ec.message();
    fs::remove(temp, ec);
    return fail(ErrorKind::Io, "cannot replace " + file.generic_string() + ": " + message);
  }
  return {};
}

Result<SimEndpoints> load_sim_endpoints(const fs::path& file, const fs::path& loaded_config, bool any) {
  auto endpoints = read_sim_endpoints(file);
  if (!endpoints || any) return endpoints;
  const auto served = canonical_or_absolute(endpoints->config);
  const auto loaded = canonical_or_absolute(loaded_config);
  if (served != loaded) {
    return refused(file, "serves " + served.generic_string() + ", not the line loaded here (" +
                             loaded.generic_string() + "); --sim-endpoints-any skips this check");
  }
  return endpoints;
}

Result<std::map<std::string, Endpoint>> load_spectrometer_endpoints(const fs::path& file,
                                                                    const fs::path& loaded_spectrometer, bool any) {
  auto endpoints = read_sim_endpoints(file);
  if (!endpoints) return fail(endpoints.error());
  if (endpoints->spectrometer.empty()) {
    return refused(file, "serves no spectrometer; start sim serve with --spectrometer " +
                             loaded_spectrometer.generic_string());
  }
  if (!any) {
    const auto served = canonical_or_absolute(endpoints->spectrometer);
    const auto loaded = canonical_or_absolute(loaded_spectrometer);
    if (served != loaded) {
      return refused(file, "serves the spectrometer " + served.generic_string() + ", not the one loaded here (" +
                               loaded.generic_string() + "); --sim-endpoints-any skips this check");
    }
  }
  return std::move(endpoints->spectrometer_transports);
}

fs::path default_sim_endpoints_path(const fs::path& line_file) {
  return line_file.parent_path() / "sim-endpoints.toml";
}

}  // namespace pychron
