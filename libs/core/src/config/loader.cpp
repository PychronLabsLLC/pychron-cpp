#include "pychron/core/config/loader.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include "pychron/core/config/validate.hpp"
#include "pychron/core/env.hpp"

namespace pychron::config {
namespace {

using Keys = std::set<std::string, std::less<>>;

std::string_view type_name(toml::node_type t) {
  switch (t) {
    case toml::node_type::none: return "nothing";
    case toml::node_type::table: return "table";
    case toml::node_type::array: return "array";
    case toml::node_type::string: return "string";
    case toml::node_type::integer: return "integer";
    case toml::node_type::floating_point: return "float";
    case toml::node_type::boolean: return "boolean";
    case toml::node_type::date: return "date";
    case toml::node_type::time: return "time";
    case toml::node_type::date_time: return "date-time";
  }
  return "unknown";
}

// Looks a key up in an optional override table first, then the main table.
using Lookup = std::function<const toml::node*(std::string_view)>;

Lookup lookup_in(const toml::table& t) {
  return [&t](std::string_view key) { return t.get(key); };
}

class Parser {
 public:
  Parser(std::string file, std::vector<Diagnostic>& out) : file_(std::move(file)), out_(out) {}

  SourceLoc loc(const toml::node& n) const {
    const auto& src = n.source();
    return SourceLoc{src.path ? std::string(*src.path) : file_, src.begin.line, src.begin.column};
  }

  void error(SourceLoc where, std::string field, std::string message) {
    out_.push_back({std::move(where), std::move(field), std::move(message)});
  }

  void begin(Located& e, const toml::node& n, std::string path) {
    e.loc = loc(n);
    e.path = std::move(path);
  }

  // Returns the node for `key`, reporting a missing required key.
  const toml::node* find(const Lookup& get, Located& e, std::string_view key, bool required) {
    const auto* n = get(key);
    if (n == nullptr) {
      if (required) error(e.loc, field(e, key), "missing required field");
      return nullptr;
    }
    e.field_locs[std::string(key)] = loc(*n);
    return n;
  }

  bool type_error(const Located& e, std::string_view key, const toml::node& n, std::string_view expected) {
    error(loc(n), field(e, key),
          "expected " + std::string(expected) + ", got " + std::string(type_name(n.type())));
    return false;
  }

  bool read(const Lookup& get, Located& e, std::string_view key, std::string& out, bool required) {
    const auto* n = find(get, e, key, required);
    if (n == nullptr) return !required;
    if (const auto* s = n->as_string()) {
      out = s->get();
      if (required && out.empty()) {
        error(loc(*n), field(e, key), "must not be empty");
        return false;
      }
      return true;
    }
    return type_error(e, key, *n, "string");
  }

  bool read(const Lookup& get, Located& e, std::string_view key, std::int64_t& out, bool required,
            std::int64_t min, std::int64_t max = INT64_MAX) {
    const auto* n = find(get, e, key, required);
    if (n == nullptr) return !required;
    const auto* i = n->as_integer();
    if (i == nullptr) return type_error(e, key, *n, "integer");
    if (i->get() < min || i->get() > max) {
      std::string range = max == INT64_MAX ? ">= " + std::to_string(min)
                                           : "in [" + std::to_string(min) + ", " + std::to_string(max) + "]";
      error(loc(*n), field(e, key), "value " + std::to_string(i->get()) + " out of range, must be " + range);
      return false;
    }
    out = i->get();
    return true;
  }

  bool read(const Lookup& get, Located& e, std::string_view key, bool& out) {
    const auto* n = find(get, e, key, false);
    if (n == nullptr) return true;
    if (const auto* b = n->as_boolean()) {
      out = b->get();
      return true;
    }
    return type_error(e, key, *n, "boolean");
  }

  bool read(const Lookup& get, Located& e, std::string_view key, std::optional<double>& out) {
    const auto* n = find(get, e, key, false);
    if (n == nullptr) return true;
    if (const auto* f = n->as_floating_point()) {
      out = f->get();
      return true;
    }
    if (const auto* i = n->as_integer()) {
      out = static_cast<double>(i->get());
      return true;
    }
    return type_error(e, key, *n, "number");
  }

  template <class T>
  bool read_array(const Lookup& get, Located& e, std::string_view key, std::vector<T>& out) {
    const auto* n = find(get, e, key, false);
    if (n == nullptr) return true;
    const auto* arr = n->as_array();
    if (arr == nullptr) return type_error(e, key, *n, "array");
    bool ok = true;
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto& el = *arr->get(i);
      const auto item_key = std::string(key) + "[" + std::to_string(i) + "]";
      e.field_locs[item_key] = loc(el);
      if (auto v = el.value<T>(); v && el.is<T>()) {
        out.push_back(*v);
      } else {
        ok = type_error(e, item_key, el, std::is_same_v<T, std::string> ? "string" : "integer");
      }
    }
    return ok;
  }

  // An enum-valued string key: maps `names[i]` to `values[i]`.
  template <class E, std::size_t N>
  bool read_enum(const Lookup& get, Located& e, std::string_view key, E& out, bool required,
                 const std::array<std::pair<std::string_view, E>, N>& table) {
    std::string s;
    if (!read(get, e, key, s, required)) return false;
    if (s.empty() && !required) return true;
    for (const auto& [name, value] : table) {
      if (name == s) {
        out = value;
        return true;
      }
    }
    std::string allowed;
    for (const auto& [name, value] : table) allowed += (allowed.empty() ? "" : " | ") + std::string(name);
    error(e.where(std::string(key)), field(e, key), "invalid value '" + s + "' (expected " + allowed + ")");
    return false;
  }

  void reject_unknown(const toml::table& t, const Located& e, const Keys& allowed) {
    for (auto&& [k, v] : t) {
      if (!allowed.contains(k.str())) error(loc(v), field(e, k.str()), "unknown field");
    }
  }

  const toml::table* as_table(const toml::node& n, const std::string& path) {
    if (const auto* t = n.as_table()) return t;
    error(loc(n), path, "expected table, got " + std::string(type_name(n.type())));
    return nullptr;
  }

  static std::string field(const Located& e, std::string_view key) {
    return e.path.empty() ? std::string(key) : e.path + "." + std::string(key);
  }

 private:
  std::string file_;
  std::vector<Diagnostic>& out_;
};

constexpr std::array kTransportKinds{
    std::pair<std::string_view, TransportKind>{"serial", TransportKind::Serial},
    std::pair<std::string_view, TransportKind>{"tcp", TransportKind::Tcp},
    std::pair<std::string_view, TransportKind>{"udp", TransportKind::Udp},
    std::pair<std::string_view, TransportKind>{"modbus_rtu", TransportKind::ModbusRtu},
    std::pair<std::string_view, TransportKind>{"modbus_tcp", TransportKind::ModbusTcp},
    std::pair<std::string_view, TransportKind>{"sim", TransportKind::Sim},
    std::pair<std::string_view, TransportKind>{"link", TransportKind::Link},
};

constexpr std::array kParities{
    std::pair<std::string_view, Parity>{"none", Parity::None},
    std::pair<std::string_view, Parity>{"even", Parity::Even},
    std::pair<std::string_view, Parity>{"odd", Parity::Odd},
};

constexpr std::array kUnits{
    std::pair<std::string_view, PressureUnits>{"torr", PressureUnits::Torr},
    std::pair<std::string_view, PressureUnits>{"mbar", PressureUnits::Mbar},
    std::pair<std::string_view, PressureUnits>{"pa", PressureUnits::Pa},
};

constexpr std::array kLevels{
    std::pair<std::string_view, LogLevel>{"trace", LogLevel::Trace},
    std::pair<std::string_view, LogLevel>{"debug", LogLevel::Debug},
    std::pair<std::string_view, LogLevel>{"info", LogLevel::Info},
    std::pair<std::string_view, LogLevel>{"warn", LogLevel::Warn},
    std::pair<std::string_view, LogLevel>{"error", LogLevel::Error},
};

// Expands a leading "~/" (or a bare "~") using $HOME, or %USERPROFILE% on Windows.
std::string expand_home(const std::string& s) {
  if (s != "~" && s.rfind("~/", 0) != 0) return s;
#ifdef _WIN32
  const auto home = env_var("USERPROFILE");
#else
  const auto home = env_var("HOME");
#endif
  if (!home || home->empty()) return s;
  return *home + s.substr(1);
}

constexpr std::array<std::string_view, 6> kOverridableTransportKeys{"port",      "host",      "baud",
                                                                    "data_bits", "stop_bits", "parity"};

class ConfigBuilder {
 public:
  ConfigBuilder(std::string file, std::vector<Diagnostic>& out) : p_(file, out), file_(std::move(file)) {}

  SystemConfig build(const toml::table& root, const toml::table* local) {
    SystemConfig c;
    c.source_file = file_;
    Located root_loc;
    p_.begin(root_loc, root, "");
    p_.reject_unknown(root,
                      root_loc,
                      Keys{"system", "transports", "drivers", "valves", "manual_valves", "switches", "gauges",
                           "pipettes", "cryo", "logging", "aliases"});

    if (const auto* s = root.get("system")) {
      if (const auto* t = p_.as_table(*s, "system")) parse_system(*t, c.system);
    } else {
      p_.error(SourceLoc{file_, 1, 1}, "system", "missing required table [system]");
    }

    if (const auto* l = root.get("logging")) {
      if (const auto* t = p_.as_table(*l, "logging")) parse_logging(*t, c.logging);
    }

    const toml::table* local_transports = local ? check_local(*local, root) : nullptr;
    for_each_named(root, "transports", [&](const std::string& name, const toml::table& t) {
      const toml::table* ovr = nullptr;
      if (local_transports != nullptr) {
        if (const auto* n = local_transports->get(name)) ovr = n->as_table();
      }
      c.transports.emplace(name, parse_transport(name, t, ovr));
    });
    for_each_named(root, "drivers", [&](const std::string& name, const toml::table& t) {
      c.drivers.emplace(name, parse_driver(name, t));
    });
    for_each_item(root, "valves", [&](const std::string& path, const toml::table& t) {
      c.valves.push_back(parse_valve(path, t));
    });
    for_each_item(root, "manual_valves", [&](const std::string& path, const toml::table& t) {
      c.manual_valves.push_back(parse_manual_valve(path, t));
    });
    for_each_item(root, "switches", [&](const std::string& path, const toml::table& t) {
      c.switches.push_back(parse_switch(path, t));
    });
    for_each_item(root, "gauges", [&](const std::string& path, const toml::table& t) {
      c.gauges.push_back(parse_gauge(path, t));
    });
    for_each_item(root, "pipettes", [&](const std::string& path, const toml::table& t) {
      c.pipettes.push_back(parse_pipette(path, t));
    });
    if (const auto* n = root.get("aliases")) {
      if (const auto* t = p_.as_table(*n, "aliases")) parse_aliases(*t, "", c.aliases);
    }
    if (const auto* n = root.get("cryo")) {
      if (const auto* t = p_.as_table(*n, "cryo")) c.cryo = parse_cryo(*t);
    }
    return c;
  }

 private:
  template <class F>
  void for_each_named(const toml::table& root, const char* section, F&& f) {
    const auto* n = root.get(section);
    if (n == nullptr) return;
    const auto* t = p_.as_table(*n, section);
    if (t == nullptr) return;
    for (auto&& [k, v] : *t) {
      const auto path = std::string(section) + "." + std::string(k.str());
      if (const auto* entry = p_.as_table(v, path)) f(std::string(k.str()), *entry);
    }
  }

  template <class F>
  void for_each_item(const toml::table& root, const char* section, F&& f) {
    const auto* n = root.get(section);
    if (n == nullptr) return;
    const auto* arr = n->as_array();
    if (arr == nullptr) {
      p_.error(p_.loc(*n), section, "expected array of tables ([[" + std::string(section) + "]])");
      return;
    }
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto path = std::string(section) + "[" + std::to_string(i) + "]";
      if (const auto* entry = p_.as_table(*arr->get(i), path)) f(path, *entry);
    }
  }

  // Validates the shape of a *.local.toml; returns its [transports] table.
  const toml::table* check_local(const toml::table& local, const toml::table& root) {
    const toml::table* transports = nullptr;
    const auto* main_transports = root.get_as<toml::table>("transports");
    for (auto&& [k, v] : local) {
      if (k.str() != "transports") {
        p_.error(p_.loc(v), std::string(k.str()), "local override may only set [transports.<name>] keys");
        continue;
      }
      transports = p_.as_table(v, "transports");
    }
    if (transports == nullptr) return nullptr;
    for (auto&& [name, node] : *transports) {
      const auto path = "transports." + std::string(name.str());
      const auto* t = p_.as_table(node, path);
      if (t == nullptr) continue;
      if (main_transports == nullptr || main_transports->get(name.str()) == nullptr) {
        p_.error(p_.loc(node), path, "local override targets unknown transport '" + std::string(name.str()) + "'");
        continue;
      }
      for (auto&& [key, value] : *t) {
        if (!is_overridable_transport_key(key.str())) {
          p_.error(p_.loc(value), path + "." + std::string(key.str()),
                   "key may not be overridden locally (allowed: port, host, baud, data_bits, stop_bits, parity)");
        }
      }
    }
    return transports;
  }

  // Flattens nested tables to dotted keys; leaves are scalars.
  void parse_aliases(const toml::table& t, const std::string& prefix, std::map<std::string, AliasConfig>& out) {
    for (auto&& [k, v] : t) {
      const std::string key = prefix.empty() ? std::string(k.str()) : prefix + "." + std::string(k.str());
      if (const auto* sub = v.as_table()) {
        parse_aliases(*sub, key, out);
        continue;
      }
      AliasConfig a;
      a.key = key;
      a.path = "aliases." + key;
      a.loc = p_.loc(v);
      if (const auto* s = v.as_string()) {
        a.value = s->get();
      } else if (const auto* i = v.as_integer()) {
        a.value = i->get();
      } else if (const auto* f = v.as_floating_point()) {
        a.value = f->get();
      } else if (const auto* b = v.as_boolean()) {
        a.value = b->get();
      } else {
        p_.error(a.loc, a.path,
                 "expected string, number or boolean, got " + std::string(type_name(v.type())));
        continue;
      }
      out.emplace(key, std::move(a));
    }
  }

  void parse_system(const toml::table& t, SystemSection& s) {
    p_.begin(s, t, "system");
    const auto get = lookup_in(t);
    p_.reject_unknown(t, s, Keys{"name", "scan_interval_ms"});
    p_.read(get, s, "name", s.name, true);
    p_.read(get, s, "scan_interval_ms", s.scan_interval_ms, false, 1);
  }

  void parse_logging(const toml::table& t, LoggingConfig& l) {
    p_.begin(l, t, "logging");
    const auto get = lookup_in(t);
    p_.reject_unknown(t, l, Keys{"dir", "max_size_mb", "max_files", "default_level", "levels", "echo_stderr"});
    std::string dir;
    if (p_.read(get, l, "dir", dir, false)) l.dir = expand_home(dir);
    p_.read(get, l, "max_size_mb", l.max_size_mb, false, 1);
    p_.read(get, l, "max_files", l.max_files, false, 1);
    p_.read_enum(get, l, "default_level", l.default_level, false, kLevels);
    p_.read(get, l, "echo_stderr", l.echo_stderr);

    const auto* n = p_.find(get, l, "levels", false);
    if (n == nullptr) return;
    const auto* lv = p_.as_table(*n, "logging.levels");
    if (lv == nullptr) return;
    for (auto&& [glob, node] : *lv) {
      const auto key = std::string(glob.str());
      const auto field = "logging.levels." + key;
      const auto* s = node.as_string();
      if (s == nullptr) {
        p_.error(p_.loc(node), field, "expected string, got " + std::string(type_name(node.type())));
        continue;
      }
      LogLevel level = LogLevel::Info;
      bool found = false;
      for (const auto& [name, value] : kLevels) {
        if (name == s->get()) {
          level = value;
          found = true;
        }
      }
      if (!found) {
        p_.error(p_.loc(node), field,
                 "invalid value '" + s->get() + "' (expected trace | debug | info | warn | error)");
        continue;
      }
      l.levels.emplace_back(key, level);
    }
  }

  TransportConfig parse_transport(const std::string& name, const toml::table& t, const toml::table* ovr) {
    TransportConfig tc;
    tc.name = name;
    p_.begin(tc, t, "transports." + name);
    const Lookup get = [&t, ovr](std::string_view key) -> const toml::node* {
      if (ovr != nullptr && is_overridable_transport_key(key)) {
        if (const auto* n = ovr->get(key)) return n;
      }
      return t.get(key);
    };
    if (!p_.read_enum(get, tc, "kind", tc.kind, true, kTransportKinds)) return tc;
    p_.read(get, tc, "timeout_ms", tc.timeout_ms, false, 1);
    p_.read(get, tc, "retries", tc.retries, false, 0);
    p_.read(get, tc, "trace", tc.trace);

    Keys allowed{"kind", "timeout_ms", "retries", "trace"};
    auto serial = [&](SerialParams& sp) {
      allowed.insert({"port", "baud", "data_bits", "stop_bits", "parity"});
      p_.read(get, tc, "port", sp.port, true);
      p_.read(get, tc, "baud", sp.baud, false, 1);
      p_.read(get, tc, "data_bits", sp.data_bits, false, 5, 8);
      p_.read(get, tc, "stop_bits", sp.stop_bits, false, 1, 2);
      p_.read_enum(get, tc, "parity", sp.parity, false, kParities);
    };
    auto tcp = [&](TcpParams& tp, bool port_required) {
      allowed.insert({"host", "port"});
      p_.read(get, tc, "host", tp.host, true);
      p_.read(get, tc, "port", tp.port, port_required, 1, 65535);
    };
    switch (tc.kind) {
      case TransportKind::Serial: {
        SerialParams sp;
        serial(sp);
        tc.params = sp;
        break;
      }
      case TransportKind::Tcp: {
        TcpParams tp;
        tcp(tp, true);
        tc.params = tp;
        break;
      }
      case TransportKind::Udp: {
        TcpParams tp;
        tcp(tp, true);
        tc.params = UdpParams{tp.host, tp.port};
        break;
      }
      case TransportKind::ModbusRtu: {
        ModbusRtuParams mp;
        serial(mp.serial);
        tc.params = mp;
        break;
      }
      case TransportKind::ModbusTcp: {
        ModbusTcpParams mp;
        mp.tcp.port = 502;
        tcp(mp.tcp, false);
        tc.params = mp;
        break;
      }
      case TransportKind::Sim:
        tc.params = SimParams{};
        break;
      case TransportKind::Link: {
        allowed.insert("link");
        LinkParams lp;
        p_.read(get, tc, "link", lp.link, true);
        tc.params = lp;
        break;
      }
    }
    p_.reject_unknown(t, tc, allowed);
    if (ovr != nullptr) {
      for (auto&& [k, v] : *ovr) {
        if (is_overridable_transport_key(k.str()) && !allowed.contains(k.str())) {
          p_.error(p_.loc(v), Parser::field(tc, k.str()), "unknown field for this transport kind");
        }
      }
    }
    return tc;
  }

  DriverConfig parse_driver(const std::string& name, const toml::table& t) {
    DriverConfig d;
    d.name = name;
    p_.begin(d, t, "drivers." + name);
    const auto get = lookup_in(t);
    p_.read(get, d, "kind", d.kind, true);
    p_.read(get, d, "transport", d.transport, true);
    p_.read_array(get, d, "channels", d.channels);
    d.options = t;  // driver-specific keys are checked by the driver's registry schema
    return d;
  }

  ValveConfig parse_valve(const std::string& path, const toml::table& t) {
    ValveConfig v;
    p_.begin(v, t, path);
    const auto get = lookup_in(t);
    p_.reject_unknown(t, v,
                      Keys{"name", "description", "actuator", "address", "interlocks", "positive_interlocks",
                           "settle_ms", "inverted", "state_source", "verify"});
    p_.read(get, v, "name", v.name, true);
    p_.read(get, v, "description", v.description, false);
    p_.read(get, v, "actuator", v.actuator, true);
    // Addresses are strings ("1", "A3"); accept a bare integer for convenience.
    read_address(get, v, v.address);
    p_.read_array(get, v, "interlocks", v.interlocks);
    p_.read_array(get, v, "positive_interlocks", v.positive_interlocks);
    p_.read(get, v, "settle_ms", v.settle_ms, false, 0);
    read_actuation(get, v, v.inverted, v.state_source, v.verify);
    return v;
  }

  // `address` as a string, or a bare integer for convenience.
  void read_address(const Lookup& get, Located& e, std::string& out) {
    if (const auto* n = get("address"); n != nullptr && n->is_integer()) {
      e.field_locs["address"] = p_.loc(*n);
      out = std::to_string(n->as_integer()->get());
    } else {
      p_.read(get, e, "address", out, true);
    }
  }

  // inverted, state_source, verify (valves and switches).
  void read_actuation(const Lookup& get, Located& e, bool& inverted, std::optional<StateSourceConfig>& source,
                      bool& verify) {
    p_.read(get, e, "inverted", inverted);
    p_.read(get, e, "verify", verify);
    const auto* n = get("state_source");
    if (n == nullptr) return;
    const auto path = Parser::field(e, "state_source");
    const auto* t = p_.as_table(*n, path);
    if (t == nullptr) return;
    StateSourceConfig s;
    p_.begin(s, *t, path);
    const auto sget = lookup_in(*t);
    p_.reject_unknown(*t, s, Keys{"driver", "address", "inverted"});
    p_.read(sget, s, "driver", s.driver, true);
    read_address(sget, s, s.address);
    p_.read(sget, s, "inverted", s.inverted);
    source = std::move(s);
  }

  SwitchConfig parse_switch(const std::string& path, const toml::table& t) {
    SwitchConfig s;
    p_.begin(s, t, path);
    const auto get = lookup_in(t);
    p_.reject_unknown(t, s,
                      Keys{"name", "description", "actuator", "address", "settle_ms", "inverted", "state_source",
                           "verify"});
    p_.read(get, s, "name", s.name, true);
    p_.read(get, s, "description", s.description, false);
    p_.read(get, s, "actuator", s.actuator, true);
    read_address(get, s, s.address);
    p_.read(get, s, "settle_ms", s.settle_ms, false, 0);
    read_actuation(get, s, s.inverted, s.state_source, s.verify);
    return s;
  }

  ManualValveConfig parse_manual_valve(const std::string& path, const toml::table& t) {
    ManualValveConfig m;
    p_.begin(m, t, path);
    const auto get = lookup_in(t);
    p_.reject_unknown(t, m, Keys{"name", "description"});
    p_.read(get, m, "name", m.name, true);
    p_.read(get, m, "description", m.description, false);
    return m;
  }

  GaugeConfig parse_gauge(const std::string& path, const toml::table& t) {
    GaugeConfig g;
    p_.begin(g, t, path);
    const auto get = lookup_in(t);
    p_.reject_unknown(t, g, Keys{"name", "driver", "channel", "units", "alarm_high", "alarm_low"});
    p_.read(get, g, "name", g.name, true);
    p_.read(get, g, "driver", g.driver, true);
    p_.read(get, g, "channel", g.channel, false, 0);
    p_.read_enum(get, g, "units", g.units, false, kUnits);
    p_.read(get, g, "alarm_high", g.alarm_high);
    p_.read(get, g, "alarm_low", g.alarm_low);
    return g;
  }

  CryoConfig parse_cryo(const toml::table& t) {
    CryoConfig cc;
    p_.begin(cc, t, "cryo");
    const auto get = lookup_in(t);
    p_.reject_unknown(t, cc, Keys{"driver", "tolerance_k", "timeout_s", "setpoints"});
    p_.read(get, cc, "driver", cc.driver, true);
    std::optional<double> tolerance, timeout;
    p_.read(get, cc, "tolerance_k", tolerance);
    p_.read(get, cc, "timeout_s", timeout);
    if (tolerance) {
      if (*tolerance > 0) cc.tolerance_k = *tolerance;
      else p_.error(p_.loc(*t.get("tolerance_k")), "cryo.tolerance_k", "must be above 0");
    }
    if (timeout) {
      if (*timeout > 0) cc.timeout_s = *timeout;
      else p_.error(p_.loc(*t.get("timeout_s")), "cryo.timeout_s", "must be above 0");
    }
    if (const auto* n = t.get("setpoints")) {
      if (const auto* table = p_.as_table(*n, "cryo.setpoints")) {
        for (auto&& [k, v] : *table) {
          const std::string field = "cryo.setpoints." + std::string(k.str());
          const auto* array = v.as_array();
          std::vector<double> values;
          bool ok = array != nullptr && !array->empty() && array->size() <= 4;
          if (ok) {
            for (const auto& e : *array) {
              auto d = e.value<double>();
              if (!d || *d < 0) {
                ok = false;
                break;
              }
              values.push_back(*d);
            }
          }
          if (!ok) {
            p_.error(p_.loc(v), field, "expected 1 to 4 kelvin values, one per output, none negative");
            continue;
          }
          cc.setpoints.emplace(std::string(k.str()), std::move(values));
        }
      }
    }
    return cc;
  }

  PipetteConfig parse_pipette(const std::string& path, const toml::table& t) {
    PipetteConfig pp;
    p_.begin(pp, t, path);
    const auto get = lookup_in(t);
    p_.reject_unknown(t, pp, Keys{"name", "inner", "outer"});
    p_.read(get, pp, "name", pp.name, true);
    p_.read(get, pp, "inner", pp.inner, true);
    p_.read(get, pp, "outer", pp.outer, true);
    return pp;
  }

  Parser p_;
  std::string file_;
};

std::optional<toml::table> parse_toml(std::string_view text, std::string_view name, std::vector<Diagnostic>& out) {
  auto result = toml::parse(text, name);
  if (!result) {
    const auto& err = result.error();
    const auto& src = err.source();
    out.push_back({SourceLoc{std::string(name), src.begin.line, src.begin.column}, "toml",
                   "syntax error: " + std::string(err.description())});
    return std::nullopt;
  }
  return std::move(result).table();
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

bool is_overridable_transport_key(std::string_view key) noexcept {
  for (auto k : kOverridableTransportKeys) {
    if (k == key) return true;
  }
  return false;
}

std::filesystem::path local_override_path(const std::filesystem::path& main_file) {
  auto out = main_file;
  out.replace_extension();
  out += ".local.toml";
  return out;
}

LoadReport load_report_from_string(std::string_view main_toml, std::string_view main_name,
                                   std::optional<std::string_view> local_toml, std::string_view local_name) {
  LoadReport report;
  auto root = parse_toml(main_toml, main_name, report.diagnostics);
  std::optional<toml::table> local;
  if (local_toml) local = parse_toml(*local_toml, local_name, report.diagnostics);
  if (!root || (local_toml && !local)) return report;

  auto config = ConfigBuilder(std::string(main_name), report.diagnostics).build(*root, local ? &*local : nullptr);
  // Cross-reference checks only run on a structurally clean file, so a missing
  // field is not also reported as a dangling reference.
  if (report.diagnostics.empty()) report.diagnostics = validate(config);
  if (report.diagnostics.empty()) report.config = std::move(config);
  return report;
}

LoadReport load_report(const std::filesystem::path& path) {
  const auto main_name = path.generic_string();
  auto text = read_file(path);
  if (!text) {
    LoadReport report;
    report.diagnostics.push_back({SourceLoc{main_name, 0, 0}, "file", "cannot read config file"});
    return report;
  }
  const auto local_path = local_override_path(path);
  std::optional<std::string> local_text;
  std::error_code ec;
  if (std::filesystem::exists(local_path, ec)) {
    local_text = read_file(local_path);
    if (!local_text) {
      LoadReport report;
      report.diagnostics.push_back({SourceLoc{local_path.generic_string(), 0, 0}, "file", "cannot read override file"});
      return report;
    }
  }
  return load_report_from_string(*text, main_name,
                                 local_text ? std::optional<std::string_view>(*local_text) : std::nullopt,
                                 local_path.generic_string());
}

Result<SystemConfig> load_system_config(const std::filesystem::path& path) {
  auto report = load_report(path);
  if (!report.ok()) return fail(to_error(report.diagnostics));
  return std::move(*report.config);
}

Result<SystemConfig> load_system_config_from_string(std::string_view main_toml, std::string_view main_name,
                                                    std::optional<std::string_view> local_toml,
                                                    std::string_view local_name) {
  auto report = load_report_from_string(main_toml, main_name, local_toml, local_name);
  if (!report.ok()) return fail(to_error(report.diagnostics));
  return std::move(*report.config);
}

}  // namespace pychron::config
