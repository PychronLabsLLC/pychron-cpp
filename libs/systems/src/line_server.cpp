#include "pychron/systems/line_server.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

#include "pychron/codecs/isotopx_ngx.hpp"
#include "pychron/codecs/thermo_qtegra.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/sim/spectrometer/wire_beam.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"

namespace pychron::systems {

namespace {

namespace cfg = spectrometer::cfg;

const char* kind_name(config::TransportKind kind) {
  switch (kind) {
    case config::TransportKind::Serial: return "serial";
    case config::TransportKind::Tcp: return "tcp";
    case config::TransportKind::Udp: return "udp";
    case config::TransportKind::ModbusRtu: return "modbus_rtu";
    case config::TransportKind::ModbusTcp: return "modbus_tcp";
    case config::TransportKind::Sim: return "sim";
    case config::TransportKind::Link: return "link";
  }
  return "?";
}

const char* kind_name(cfg::TransportKind kind) {
  switch (kind) {
    case cfg::TransportKind::Serial: return "serial";
    case cfg::TransportKind::Tcp: return "tcp";
    case cfg::TransportKind::Udp: return "udp";
    case cfg::TransportKind::ModbusRtu: return "modbus_rtu";
    case cfg::TransportKind::ModbusTcp: return "modbus_tcp";
    case cfg::TransportKind::LabjackU3: return "labjack_u3";
    case cfg::TransportKind::Sim: return "sim";
    case cfg::TransportKind::Link: return "link";
  }
  return "?";
}

// The address a network transport names, for the shared-address check.
std::optional<std::string> address_of(const config::TransportConfig& tc) {
  if (const auto* p = std::get_if<config::TcpParams>(&tc.params)) return p->host + ":" + std::to_string(p->port);
  if (const auto* p = std::get_if<config::UdpParams>(&tc.params)) return p->host + ":" + std::to_string(p->port);
  if (const auto* p = std::get_if<config::ModbusTcpParams>(&tc.params))
    return p->tcp.host + ":" + std::to_string(p->tcp.port);
  return std::nullopt;
}

std::optional<std::string> address_of(const cfg::TransportConfig& tc) {
  if (tc.kind == cfg::TransportKind::Tcp || tc.kind == cfg::TransportKind::Udp ||
      tc.kind == cfg::TransportKind::ModbusTcp)
    return tc.host + ":" + std::to_string(tc.tcp_port);
  return std::nullopt;
}

enum class Vendor { None, Qtegra, Ngx };

Vendor vendor_of(std::string_view kind) {
  if (kind == "thermo_qtegra" || kind == "qtegra_valves" || kind == "qtegra_gauges") return Vendor::Qtegra;
  if (kind == "isotopx_ngx" || kind == "ngx_valves") return Vendor::Ngx;
  return Vendor::None;
}

// The name a vendor driver registers its link under: its `link` option, else its name.
std::string link_name(const std::string& driver, const toml::table& options) {
  return options["link"].value_or(driver);
}

// One served transport, of the line's file or the spectrometer's.
struct Member {
  std::string name;
  bool spectrometer = false;
  std::string kind;  // the config's transport kind
  WireProtocol protocol = WireProtocol::Tcp;
  std::vector<const config::DriverConfig*> line_drivers;
  std::vector<const cfg::DriverConfig*> spectrometer_drivers;
};

// Transports one listener serves: one, or several reaching one vendor PC,
// with the line's link-borrowing drivers that talk to that PC too.
struct Group {
  std::vector<Member> members;
  std::vector<const config::DriverConfig*> joiners;  // on line kind "link" transports
};

// "<name>" for a line transport, "<name> (spectrometer)" for one of the spectrometer's.
std::string label(const Member& m) { return m.spectrometer ? m.name + " (spectrometer)" : m.name; }

std::vector<std::string> driver_kinds(const Group& g) {
  std::vector<std::string> kinds;
  auto add = [&kinds](const std::string& kind) {
    if (std::ranges::find(kinds, kind) == kinds.end()) kinds.push_back(kind);
  };
  for (const auto& m : g.members) {
    for (const auto* d : m.line_drivers) add(d->kind);
    for (const auto* d : m.spectrometer_drivers) add(d->kind);
  }
  for (const auto* d : g.joiners) add(d->kind);
  return kinds;
}

// The one vendor every driver of the group is of; None when they differ or are no vendor's.
Vendor group_vendor(const Group& g) {
  const auto kinds = driver_kinds(g);
  if (kinds.empty()) return Vendor::None;
  const Vendor first = vendor_of(kinds.front());
  for (const auto& k : kinds)
    if (vendor_of(k) != first) return Vendor::None;
  return first;
}

std::string joined(const std::vector<std::string>& kinds) {
  std::string out;
  for (const auto& k : kinds) out += (out.empty() ? "" : "+") + k;
  return out;
}

}  // namespace

LineServer::~LineServer() { stop(); }

Result<std::unique_ptr<LineServer>> LineServer::start(const config::SystemConfig& config,
                                                      const canvas::Canvas* canvas, const Clock& clock,
                                                      LineServerOptions options) {
  auto built = build_line_sim(config, canvas, clock, std::move(options.sim));
  if (!built) return fail(built.error());
  std::unique_ptr<LineServer> server(new LineServer());
  server->system_ = std::move(built->system);
  server->notes_ = std::move(built->notes);
  server->config_file_ = config.source_file;
  server->host_ = options.advertise.empty() ? options.bind : options.advertise;
  server->wire_ = std::make_unique<sim::WireServer>(
      sim::WireServer::Options{.bind = options.bind, .port_base = options.port_base, .clock = &clock});
  auto& notes = server->notes_;
  const cfg::SpectrometerConfig* spec = options.spectrometer ? &options.spectrometer->config : nullptr;

  // The beam the spectrometer's PC measures, fed by the line's gas.
  if (spec != nullptr) {
    server->spectrometer_file_ = spec->source_file;
    server->beam_ = std::make_shared<sim::BeamModel>(clock, spectrometer::beam_settings_from_config(*options.spectrometer));
    for (const auto& d : spec->detectors)
      if (auto ref = cfg::parse_channel_ref(d.channel)) server->beam_->ensure_detector(ref->channel);
    for (const auto& [name, d] : spec->drivers)
      for (const auto& c : d.channels) server->beam_->ensure_detector(c);
    auto fed = spectrometer::feed_beam_from_line(*server->beam_, *server->system_);
    if (!fed) return fail(fed.error());
    if (*fed == spectrometer::BeamFeed::FixedGas) {
      notes.push_back({LogLevel::Warn, std::string("the spectrometer is not joined to the line: ") +
                                           (canvas != nullptr ? "the canvas has no spectrometer stage"
                                                              : "the line was loaded without its canvas")});
    }
  }

  // Served transports, grouped by the address they name.
  std::vector<Group> groups;
  std::map<std::string, std::size_t> by_address;
  auto place = [&](Member m, const std::optional<std::string>& address) {
    if (address) {
      if (const auto it = by_address.find(*address); it != by_address.end()) {
        groups[it->second].members.push_back(std::move(m));
        return;
      }
      by_address.emplace(*address, groups.size());
    }
    groups.push_back(Group{{std::move(m)}, {}});
  };
  std::vector<const config::TransportConfig*> line_links;
  for (const auto& [name, tc] : config.transports) {
    switch (tc.kind) {
      case config::TransportKind::Sim:
        server->in_process_.push_back(name);
        continue;
      case config::TransportKind::Link:
        line_links.push_back(&tc);
        continue;
      case config::TransportKind::ModbusRtu:
        notes.push_back({LogLevel::Warn, name + ": modbus_rtu is not supported: not served"});
        continue;
      case config::TransportKind::Serial:
        notes.push_back({LogLevel::Info, name + ": serial served as TCP; baud, parity and port name are not exercised"});
        break;
      case config::TransportKind::Tcp:
      case config::TransportKind::Udp:
      case config::TransportKind::ModbusTcp:
        break;
    }
    Member m{name, false, kind_name(tc.kind),
             tc.kind == config::TransportKind::Udp ? WireProtocol::Udp : WireProtocol::Tcp, {}, {}};
    for (const auto& [_, d] : config.drivers)
      if (d.transport == name) m.line_drivers.push_back(&d);
    place(std::move(m), address_of(tc));
  }
  if (spec != nullptr) {
    for (const auto& [name, tc] : spec->transports) {
      const std::string shown = name + " (spectrometer)";
      if (tc.kind == cfg::TransportKind::Sim) {
        server->in_process_.push_back(shown);
        continue;
      }
      if (tc.kind == cfg::TransportKind::Link) continue;
      if (tc.kind == cfg::TransportKind::ModbusRtu || tc.kind == cfg::TransportKind::LabjackU3) {
        notes.push_back({LogLevel::Warn, shown + ": " + kind_name(tc.kind) + " is not supported: not served"});
        continue;
      }
      if (tc.kind == cfg::TransportKind::Serial) {
        notes.push_back({LogLevel::Info, shown + ": serial served as TCP; baud, parity and port name are not exercised"});
      }
      Member m{name, true, kind_name(tc.kind),
               tc.kind == cfg::TransportKind::Udp ? WireProtocol::Udp : WireProtocol::Tcp, {}, {}};
      for (const auto& [_, d] : spec->drivers)
        if (d.transport == name) m.spectrometer_drivers.push_back(&d);
      place(std::move(m), address_of(tc));
    }
  }
  // A line transport of kind "link" talks to the PC whose vendor driver registers that link.
  for (const auto* link : line_links) {
    const auto* params = std::get_if<config::LinkParams>(&link->params);
    if (params == nullptr) continue;
    for (auto& g : groups) {
      bool owns = false;
      for (const auto& m : g.members) {
        for (const auto* d : m.spectrometer_drivers)
          owns = owns || (vendor_of(d->kind) != Vendor::None && link_name(d->name, d->options) == params->link);
        for (const auto* d : m.line_drivers)
          owns = owns || (vendor_of(d->kind) != Vendor::None && link_name(d->name, d->options) == params->link);
      }
      if (!owns) continue;
      for (const auto& [_, d] : config.drivers)
        if (d.transport == link->name) g.joiners.push_back(&d);
      if (group_vendor(g) == Vendor::None) {
        return fail(ErrorKind::Config, "sim serve: '" + link->name + "' borrows the link '" + params->link + "' of '" +
                                           label(g.members.front()) +
                                           "', but its drivers are not of that PC's vendor");
      }
      break;
    }
  }

  // Each group on one listener.
  for (auto& g : groups) {
    std::ranges::stable_sort(g.members, [](const Member& a, const Member& b) { return a.spectrometer < b.spectrometer; });
    const Member& first = g.members.front();
    const Vendor vendor = group_vendor(g);
    if (g.members.size() > 1 && vendor == Vendor::None) {
      return fail(ErrorKind::Config, "sim serve: transports '" + label(first) + "' and '" + label(g.members[1]) +
                                         "' name one address; one emulator answers both only when every driver on "
                                         "them is of one Qtegra or NGX PC");
    }
    const bool spectrometer_driver = std::ranges::any_of(g.members, [](const Member& m) {
      return std::ranges::any_of(m.spectrometer_drivers, [](const auto* d) { return vendor_of(d->kind) != Vendor::None; });
    });
    sim::WireSim wire;
    if (vendor == Vendor::Qtegra && (g.members.size() > 1 || !g.joiners.empty() || spectrometer_driver)) {
      auto model = std::make_shared<spectrometer::QtegraSimModel>();
      for (const auto& m : g.members) {
        for (const auto* d : m.line_drivers) server->system_->join_qtegra(*model, *d, config);
        if (!m.spectrometer_drivers.empty() && server->beam_) sim::feed_qtegra_from_beam(*model, server->beam_);
      }
      for (const auto* d : g.joiners) server->system_->join_qtegra(*model, *d, config);
      wire.hook = spectrometer::qtegra_sim_hook(model);
      wire.request = sim::framer(codec::qtegra::request_spec());
      wire.emulator = joined(driver_kinds(g));
    } else if (vendor == Vendor::Ngx && (g.members.size() > 1 || !g.joiners.empty() || spectrometer_driver)) {
      auto model = std::make_shared<spectrometer::NgxSimModel>();
      model->clock = &clock;
      model->banner_pending = false;
      std::string terminator(codec::ngx::kDefaultSendTerminator);
      for (const auto& m : g.members) {
        for (const auto* d : m.line_drivers) {
          server->system_->join_ngx(*model, *d, config);
          terminator = d->options["send_terminator"].value_or(terminator);
        }
        for (const auto* d : m.spectrometer_drivers) {
          if (d->kind != "isotopx_ngx" || !server->beam_) continue;
          sim::feed_ngx_from_beam(*model, server->beam_, d->channels);
          terminator = d->options["send_terminator"].value_or(terminator);
          // The instrument greets every connection and streams its events.
          wire.on_connect = [model] {
            const std::lock_guard lock(model->mutex);
            model->banner_pending = true;
          };
          wire.unsolicited = spectrometer::ngx_sim_events(model);
        }
      }
      for (const auto* d : g.joiners) server->system_->join_ngx(*model, *d, config);
      wire.hook = spectrometer::ngx_sim_hook(model);
      wire.request = sim::framer(ReadSpec::until(terminator.empty() ? "\r" : terminator));
      wire.emulator = joined(driver_kinds(g));
    } else if (!first.spectrometer) {
      wire = server->system_->wire_for_transport(first.name, config);
    } else {
      wire.hook = [](const Bytes&) { return Bytes{}; };
      wire.request = sim::framer(ReadSpec::until_any("\r\n"));
    }
    if (wire.emulator.empty()) {
      const auto kinds = driver_kinds(g);
      std::string said = label(first);
      if (kinds.empty()) {
        said += ": no driver uses it: silent wire";
      } else {
        said += ": no emulator for driver kind '";
        said += kinds.front();
        said += "': silent wire";
      }
      notes.push_back({LogLevel::Warn, std::move(said)});
    }
    // Spectrometer transports may share a line transport's name: their own key.
    const std::string key = first.spectrometer ? "spectrometer:" + first.name : first.name;
    for (const auto& m : g.members) {
      ServedTransport s{m.name, m.kind, first.protocol, 0, wire.emulator, m.spectrometer, {}};
      if (&m != &first) s.shares = label(first);
      server->served_.push_back(std::move(s));
    }
    if (auto added = server->wire_->add(key, first.protocol, std::move(wire)); !added) return fail(added.error());
    for (std::size_t i = 0; i < g.members.size(); ++i) server->key_of_.push_back(key);
  }
  // Making the hooks may refuse a gauge's volume.
  if (auto refused = server->system_->build_error()) return fail(*refused);

  auto endpoints = server->wire_->start();
  if (!endpoints) return fail(endpoints.error());
  for (std::size_t i = 0; i < server->served_.size(); ++i)
    server->served_[i].port = endpoints->at(server->key_of_[i]).port;
  // Line first, then the spectrometer, each in name order.
  std::ranges::stable_sort(server->served_, [](const ServedTransport& a, const ServedTransport& b) {
    return std::tie(a.spectrometer, a.name) < std::tie(b.spectrometer, b.name);
  });
  return server;
}

SimEndpoints LineServer::endpoints() const {
  auto canonical = [](const std::string& file) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(file, ec);
    return ec ? std::filesystem::path(file) : c;
  };
  SimEndpoints out;
  out.config = canonical(config_file_);
  out.host = host_;
  if (!spectrometer_file_.empty()) out.spectrometer = canonical(spectrometer_file_);
  for (const auto& s : served_) {
    (s.spectrometer ? out.spectrometer_transports : out.transports)
        .emplace(s.name, Endpoint{host_, s.port, s.protocol, s.emulator});
  }
  return out;
}

void LineServer::stop() {
  if (wire_) wire_->stop();
}

}  // namespace pychron::systems
