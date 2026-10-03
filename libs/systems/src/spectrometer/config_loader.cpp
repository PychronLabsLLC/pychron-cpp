#include "pychron/systems/spectrometer/config_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

#include "pychron/core/config/loader.hpp"
#include "toml_reader.hpp"

namespace pychron::spectrometer::cfg {
namespace {

using detail::Obj;
using detail::Reader;

template <class E, std::size_t N>
using Table = std::array<std::pair<std::string_view, E>, N>;

constexpr Table<TransportKind, 7> kTransportKinds{{
    {"tcp", TransportKind::Tcp},
    {"serial", TransportKind::Serial},
    {"modbus_tcp", TransportKind::ModbusTcp},
    {"modbus_rtu", TransportKind::ModbusRtu},
    {"labjack_u3", TransportKind::LabjackU3},
    {"sim", TransportKind::Sim},
    {"link", TransportKind::Link},
}};

constexpr Table<Axis, 3> kAxes{{{"dac", Axis::Dac}, {"field", Axis::Field}, {"mass", Axis::Mass}}};

constexpr Table<DetectorKind, 4> kDetectorKinds{{
    {"faraday", DetectorKind::Faraday},
    {"counter", DetectorKind::Counter},
    {"cdd", DetectorKind::Cdd},
    {"atona", DetectorKind::Atona},
}};

constexpr Table<BinEpoch, 2> kBinEpochs{{{"shared", BinEpoch::Shared}, {"per_acquirer", BinEpoch::PerAcquirer}}};

// Canonical source parameter names (spec section 5.1 SourceParam).
constexpr std::array<std::string_view, 19> kSourceParams{
    "HV",          "TrapCurrent",     "TrapVoltage",        "Emission",   "ElectronEnergy",
    "IonRepeller", "ExtractionLens",  "ExtractionFocus",    "ExtractionSymmetry", "YSymmetry",
    "ZSymmetry",   "ZFocus",          "HorizontalSymmetry", "Flatapole",  "RotationQuad",
    "PoleN",       "PoleS",           "ESAPlus",            "ESAMinus"};

// Keys a `*.local.toml` may set on an existing [transports.<name>].
constexpr std::array<std::string_view, 4> kOverridable{"host", "port", "baud", "timeout_ms"};

constexpr std::array<std::string_view, 8> kSections{"system", "transports", "drivers",          "magnet",
                                                    "source", "acquisition", "detector_control", "detectors"};

template <class Arr>
bool contains(const Arr& arr, std::string_view s) {
  for (auto v : arr) {
    if (v == s) return true;
  }
  return false;
}

template <class E, std::size_t N>
detail::Choices<E> choices(const Table<E, N>& t) {
  return {t.data(), t.size()};
}

bool is_hex_color(const std::string& s) {
  return s.size() == 7 && s[0] == '#' &&
         std::all_of(s.begin() + 1, s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

class Builder {
 public:
  Builder(std::string file, std::vector<config::Diagnostic>& out) : r_(std::move(file), out) {}

  SpectrometerConfig build(const toml::table& root) {
    SpectrometerConfig c;
    c.source_file = r_.file();
    const Obj top = r_.obj(root, "");
    for (auto&& [k, v] : root) {
      if (!contains(kSections, k.str())) r_.error(r_.loc(v), std::string(k.str()), "unknown section");
    }

    if (auto s = section(top, "system", &c.system)) parse_system(*s, c.system);
    each_named(top, "transports", [&](const std::string& name, const Obj& o) {
      auto& t = c.transports[name];
      t.name = name;
      parse_transport(o, t);
    });
    each_named(top, "drivers", [&](const std::string& name, const Obj& o) {
      auto& d = c.drivers[name];
      d.name = name;
      parse_driver(o, d);
    });
    if (auto s = section(top, "magnet", &c.magnet)) parse_magnet(*s, c.magnet);
    if (auto s = section(top, "source", &c.source)) parse_source(*s, c.source);
    if (auto s = section(top, "acquisition", &c.acquisition)) parse_acquisition(*s, c.acquisition);
    if (root.get("detector_control") != nullptr) {
      c.detector_control.emplace();
      if (auto s = r_.sub(top, "detector_control", false, &*c.detector_control)) {
        r_.only(*s, {"driver"});
        r_.str(*s, "driver", c.detector_control->driver, true);
      }
    }
    parse_detectors(root, c.detectors);
    return c;
  }

 private:
  std::optional<Obj> section(const Obj& top, std::string_view key, Located* rec) {
    if (top.t.get(key) == nullptr) {
      r_.error(config::SourceLoc{r_.file(), 1, 1}, std::string(key), "missing required section");
      return std::nullopt;
    }
    return r_.sub(top, key, true, rec);
  }

  template <class F>
  void each_named(const Obj& top, std::string_view key, F&& f) {
    auto s = r_.sub(top, key, false);
    if (!s) return;
    for (auto&& [name, node] : s->t) {
      const auto path = Reader::join(s->path, name.str());
      if (const auto* t = r_.table(node, path)) f(std::string(name.str()), Obj{*t, path, r_.loc(node), nullptr});
    }
  }

  static void record(const Obj& o, Located& e) {
    e.loc = o.loc;
    e.path = o.path;
  }

  void parse_system(const Obj& o, SystemSection& s) {
    r_.only(o, {"name", "reference_detector", "integration_time_s"});
    r_.str(o, "name", s.name, true);
    r_.str(o, "reference_detector", s.reference_detector, true);
    if (r_.number(o, "integration_time_s", s.integration_time_s, false))
      r_.require(s.integration_time_s > 0.0, o, "integration_time_s", "must be > 0");
  }

  void parse_transport(Obj o, TransportConfig& t) {
    record(o, t);
    o.rec = &t;
    r_.only(o, {"kind", "host", "port", "baud", "timeout_ms", "retries", "trace", "link"});
    const bool kind_ok = r_.choice(o, "kind", t.kind, choices(kTransportKinds), true) && o.t.get("kind") != nullptr;
    r_.str(o, "host", t.host, false);
    r_.integer(o, "baud", t.baud, false, 1);
    r_.integer(o, "timeout_ms", t.timeout_ms, false, 1);
    r_.integer(o, "retries", t.retries, false, 0);
    r_.boolean(o, "trace", t.trace);
    if (!kind_ok) return;
    r_.str(o, "link", t.link, t.kind == TransportKind::Link);
    if (t.kind != TransportKind::Link && o.t.get("link") != nullptr)
      r_.error(o.loc, Reader::join(o.path, "link"), "only for kind \"link\"");

    const bool network = t.kind == TransportKind::Tcp || t.kind == TransportKind::ModbusTcp;
    const bool serial = t.kind == TransportKind::Serial || t.kind == TransportKind::ModbusRtu;
    if (const auto* port = o.t.get("port")) {
      if (port->is_integer() && !serial) {
        r_.integer(o, "port", t.tcp_port, true, 1);
      } else if (port->is_string() && !network) {
        r_.str(o, "port", t.serial_port, true);
      } else {
        r_.error(r_.loc(*port), Reader::join(o.path, "port"),
                 network ? "expected integer TCP port" : "expected device path string");
      }
    } else if (serial) {
      r_.error(o.loc, Reader::join(o.path, "port"), "missing required field");
    }
    if (network && t.host.empty()) r_.error(o.loc, Reader::join(o.path, "host"), "missing required field");
  }

  void parse_driver(Obj o, DriverConfig& d) {
    record(o, d);
    o.rec = &d;
    r_.str(o, "kind", d.kind, true);
    r_.str(o, "transport", d.transport, true);
    std::vector<std::string> roles;
    if (r_.strings(o, "roles", roles, true) && o.t.get("roles") != nullptr) {
      if (roles.empty()) r_.require(false, o, "roles", "must list at least one role");
      for (std::size_t i = 0; i < roles.size(); ++i) {
        if (auto role = role_from_string(roles[i])) {
          d.roles.push_back(*role);
        } else {
          r_.error(r_.loc(*o.t.get("roles")->as_array()->get(i)), Reader::join(o.path, "roles") + "[" + std::to_string(i) + "]",
                   "unknown role '" + roles[i] + "' (expected positioner, source, acquirer, detector_control, beam_blank)");
        }
      }
    }
    r_.strings(o, "channels", d.channels, false);
    for (auto&& [k, v] : o.t) {
      const auto key = k.str();
      if (key == "kind" || key == "transport" || key == "roles" || key == "channels") continue;
      v.visit([&](auto&& value) { d.options.insert_or_assign(key, value); });
    }
  }

  void parse_magnet(const Obj& o, MagnetSection& m) {
    r_.only(o, {"positioner", "native_axis", "limits", "settle_ms", "field_table", "hv_table", "propagate",
                "corrections", "protection", "af_demag"});
    r_.str(o, "positioner", m.positioner, true);
    r_.choice(o, "native_axis", m.native_axis, choices(kAxes), true);
    if (auto l = r_.sub(o, "limits", false)) {
      Limits lim;
      r_.only(*l, {"min", "max"});
      if (r_.number(*l, "min", lim.min, true) && r_.number(*l, "max", lim.max, true)) {
        if (lim.min < lim.max) {
          m.limits = lim;
        } else {
          r_.error(l->loc, l->path, "min must be < max");
        }
      }
    }
    r_.integer(o, "settle_ms", m.settle_ms, false, 0);
    r_.str(o, "field_table", m.field_table, true);
    r_.str(o, "hv_table", m.hv_table, false);
    r_.boolean(o, "propagate", m.propagate);
    if (auto s = r_.sub(o, "corrections", false)) {
      r_.only(*s, {"deflection", "hv"});
      r_.boolean(*s, "deflection", m.corrections.deflection);
      r_.boolean(*s, "hv", m.corrections.hv);
    }
    if (auto s = r_.sub(o, "protection", false)) {
      r_.only(*s, {"detectors", "beam_blank_threshold"});
      r_.strings(*s, "detectors", m.protection.detectors, false);
      if (r_.number(*s, "beam_blank_threshold", m.protection.beam_blank_threshold) && m.protection.beam_blank_threshold)
        r_.require(*m.protection.beam_blank_threshold > 0.0, *s, "beam_blank_threshold", "must be > 0");
    }
    if (auto s = r_.sub(o, "af_demag", false)) {
      auto& a = m.af_demag;
      r_.only(*s, {"enabled", "period_s", "duration_s", "start_amplitude", "threshold"});
      r_.boolean(*s, "enabled", a.enabled);
      if (r_.number(*s, "period_s", a.period_s, false)) r_.require(a.period_s > 0.0, *s, "period_s", "must be > 0");
      if (r_.number(*s, "duration_s", a.duration_s, false))
        r_.require(a.duration_s > 0.0, *s, "duration_s", "must be > 0");
      r_.number(*s, "start_amplitude", a.start_amplitude, false);
      r_.number(*s, "threshold", a.threshold, false);
    }
  }

  void parse_source(const Obj& o, SourceSection& s) {
    r_.only(o, {"driver", "nominal_hv", "ramp"});
    r_.str(o, "driver", s.driver, true);
    if (r_.number(o, "nominal_hv", s.nominal_hv) && s.nominal_hv)
      r_.require(*s.nominal_hv > 0.0, o, "nominal_hv", "must be > 0");
    if (auto ramp = r_.sub(o, "ramp", false)) {
      for (auto&& [k, v] : ramp->t) {
        const auto key = std::string(k.str());
        if (!contains(kSourceParams, key)) {
          r_.error(r_.loc(v), Reader::join(ramp->path, key), "unknown source parameter '" + key + "'");
          continue;
        }
        double rate = 0.0;
        if (!r_.number(*ramp, key, rate, true)) continue;
        if (rate > 0.0) {
          s.ramp[key] = rate;
        } else {
          r_.error(r_.loc(v), Reader::join(ramp->path, key), "ramp rate must be > 0");
        }
      }
    }
  }

  void parse_acquisition(const Obj& o, AcquisitionSection& a) {
    r_.only(o, {"acquirers", "stale_frame_guard", "timeout_factor", "host_integration", "bin_epoch",
                "ignored_channels"});
    if (r_.strings(o, "acquirers", a.acquirers, true) && o.t.get("acquirers") != nullptr)
      r_.require(!a.acquirers.empty(), o, "acquirers", "must list at least one acquirer driver");
    r_.boolean(o, "stale_frame_guard", a.stale_frame_guard);
    if (r_.number(o, "timeout_factor", a.timeout_factor, false))
      r_.require(a.timeout_factor >= 1.0, o, "timeout_factor", "must be >= 1");
    r_.boolean(o, "host_integration", a.host_integration);
    r_.choice(o, "bin_epoch", a.bin_epoch, choices(kBinEpochs), false);
    if (r_.strings(o, "ignored_channels", a.ignored_channels, false)) {
      for (std::size_t i = 0; i < a.ignored_channels.size(); ++i) {
        if (!parse_channel_ref(a.ignored_channels[i])) {
          r_.error(r_.loc(*o.t.get("ignored_channels")->as_array()->get(i)),
                   "acquisition.ignored_channels[" + std::to_string(i) + "]", "expected \"<driver>:<channel>\"");
        }
      }
    }
  }

  void parse_detectors(const toml::table& root, std::vector<DetectorConfig>& out) {
    const auto* n = root.get("detectors");
    const auto* arr = n != nullptr ? n->as_array() : nullptr;
    if (n != nullptr && arr == nullptr) {
      r_.error(r_.loc(*n), "detectors", "expected array of tables ([[detectors]])");
      return;
    }
    if (arr == nullptr || arr->empty()) {
      r_.error(config::SourceLoc{r_.file(), 1, 1}, "detectors", "at least one detector ([[detectors]]) is required");
      return;
    }
    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto path = "detectors[" + std::to_string(i) + "]";
      const auto* t = r_.table(*arr->get(i), path);
      if (t == nullptr) continue;
      auto& d = out.emplace_back();
      parse_detector(r_.obj(*t, path, &d), d);
    }
  }

  void parse_detector(const Obj& o, DetectorConfig& d) {
    r_.only(o, {"name", "kind", "channel", "units", "software_gain", "isotope", "color", "active", "deflection", "protection",
                "saturation", "dead_time_ns", "cdd_voltage"});
    r_.str(o, "name", d.name, true);
    r_.choice(o, "kind", d.kind, choices(kDetectorKinds), true);
    if (r_.str(o, "channel", d.channel, true) && !d.channel.empty())
      r_.require(parse_channel_ref(d.channel).has_value(), o, "channel", "expected \"<driver>:<channel>\"");
    const bool counting = d.kind == DetectorKind::Counter || d.kind == DetectorKind::Cdd;
    d.units = counting ? "cps" : "fA";
    r_.str(o, "units", d.units, false);
    if (r_.number(o, "software_gain", d.software_gain, false))
      r_.require(d.software_gain > 0.0, o, "software_gain", "must be > 0");
    r_.str(o, "isotope", d.isotope, false);
    if (r_.str(o, "color", d.color, false) && !d.color.empty()) {
      r_.require(is_hex_color(d.color), o, "color", "expected \"#rrggbb\"");
      for (auto& c : d.color) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    r_.boolean(o, "active", d.active);

    if (auto s = r_.sub(o, "deflection", false)) {
      auto& f = d.deflection.emplace();
      r_.only(*s, {"control", "correction", "sign", "max", "per_volt"});
      r_.boolean(*s, "control", f.control);
      r_.numbers(*s, "correction", f.correction);
      std::int64_t sign = 1;
      if (r_.integer(*s, "sign", sign, false, INT64_MIN)) {
        r_.require(sign == 1 || sign == -1, *s, "sign", "must be 1 or -1");
        f.sign = sign < 0 ? -1 : 1;
      }
      if (r_.number(*s, "max", f.max) && f.max) r_.require(*f.max > 0.0, *s, "max", "must be > 0");
      r_.number(*s, "per_volt", f.per_volt);
    }
    if (auto s = r_.sub(o, "protection", false)) {
      auto& p = d.protection.emplace();
      r_.only(*s, {"threshold", "on_move"});
      if (r_.number(*s, "threshold", p.threshold, true) && s->t.get("threshold") != nullptr)
        r_.require(p.threshold > 0.0, *s, "threshold", "must be > 0");
      r_.boolean(*s, "on_move", p.on_move);
    }
    if (r_.number(o, "saturation", d.saturation) && d.saturation)
      r_.require(*d.saturation > 0.0, o, "saturation", "must be > 0");
    if (r_.number(o, "dead_time_ns", d.dead_time_ns) && d.dead_time_ns)
      r_.require(*d.dead_time_ns >= 0.0, o, "dead_time_ns", "must be >= 0");
    r_.number(o, "cdd_voltage", d.cdd_voltage);
  }

  Reader r_;
};

// Validates a *.local.toml's shape and merges its transport keys into `root`.
void merge_local(toml::table& root, const toml::table& local, Reader& r) {
  auto* transports = root.get_as<toml::table>("transports");
  for (auto&& [k, v] : local) {
    if (k.str() != "transports") {
      r.error(r.loc(v), std::string(k.str()), "local override may only set [transports.<name>] keys");
      continue;
    }
    const auto* lt = r.table(v, "transports");
    if (lt == nullptr) continue;
    for (auto&& [name, node] : *lt) {
      const auto path = "transports." + std::string(name.str());
      const auto* t = r.table(node, path);
      if (t == nullptr) continue;
      auto* target = transports != nullptr ? transports->get_as<toml::table>(name.str()) : nullptr;
      if (target == nullptr) {
        r.error(r.loc(node), path, "local override targets unknown transport '" + std::string(name.str()) + "'");
        continue;
      }
      for (auto&& [key, value] : *t) {
        if (!contains(kOverridable, key.str())) {
          r.error(r.loc(value), path + "." + std::string(key.str()),
                  "key may not be overridden locally (allowed: host, port, baud, timeout_ms)");
          continue;
        }
        value.visit([&](auto&& x) { target->insert_or_assign(key.str(), x); });
      }
    }
  }
}

}  // namespace

ConfigLoadReport parse_config_from_string(std::string_view main_toml, std::string_view main_name,
                                          std::optional<std::string_view> local_toml, std::string_view local_name) {
  ConfigLoadReport report;
  auto root = detail::parse_toml(main_toml, main_name, report.diagnostics);
  std::optional<toml::table> local;
  if (local_toml) local = detail::parse_toml(*local_toml, local_name, report.diagnostics);
  if (!root || (local_toml && !local)) return report;

  if (local) {
    Reader r(std::string(local_name), report.diagnostics);
    merge_local(*root, *local, r);
  }
  auto config = Builder(std::string(main_name), report.diagnostics).build(*root);
  if (report.diagnostics.empty()) report.config = std::move(config);
  return report;
}

ConfigLoadReport parse_config(const std::filesystem::path& path) {
  const auto main_name = path.generic_string();
  auto text = detail::read_file(path);
  if (!text) {
    ConfigLoadReport report;
    report.diagnostics.push_back({config::SourceLoc{main_name, 0, 0}, "file", "cannot read config file"});
    return report;
  }
  const auto local_path = config::local_override_path(path);
  std::error_code ec;
  if (!std::filesystem::exists(local_path, ec)) return parse_config_from_string(*text, main_name);
  auto local_text = detail::read_file(local_path);
  if (!local_text) {
    ConfigLoadReport report;
    report.diagnostics.push_back(
        {config::SourceLoc{local_path.generic_string(), 0, 0}, "file", "cannot read override file"});
    return report;
  }
  return parse_config_from_string(*text, main_name, *local_text, local_path.generic_string());
}

}  // namespace pychron::spectrometer::cfg
