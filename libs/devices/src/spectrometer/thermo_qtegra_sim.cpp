#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"

#include <algorithm>
#include <optional>

#include "pychron/codecs/thermo_qtegra.hpp"

namespace pychron::spectrometer {

namespace q = codec::qtegra;

namespace {

// Value stored under `key`, 0 when never set.
double value_or_zero(const std::map<std::string, double>& values, const std::string& key) {
  const auto it = values.find(key);
  return it == values.end() ? 0.0 : it->second;
}

std::optional<bool> parse_switch(const std::string& text, std::string_view on, std::string_view off) {
  if (text == on) return true;
  if (text == off) return false;
  return std::nullopt;
}

// A parameter reads back what was set under that name; a readback name
// nothing was stored under reports its set name's value.
double parameter(const QtegraSimModel& m, const std::string& name) {
  if (const auto it = m.params.find(name); it != m.params.end()) return it->second;
  if (const auto canonical = q::canonical_name(name)) {
    if (const auto set_name = q::hardware_name(*canonical)) return value_or_zero(m.params, std::string(*set_name));
  }
  return 0.0;
}

Bytes describe(const QtegraSimModel& m) {
  q::ServerInfo server;
  server.protocol = q::kProtocolVersion;
  server.version = "sim";
  server.instrument = "SimQtegra";
  server.transport = "tcp";
  server.tag_data = m.tag_data;
  server.features = {"keepalive", "describe"};
  std::vector<std::string> names = m.detectors;
  if (names.empty()) {
    names.assign(q::kDefaultDetectorOrder.begin(), q::kDefaultDetectorOrder.end());
    for (const auto& [name, value] : m.intensities) {
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
  }
  for (const auto& name : names) server.detectors.push_back({"CUP " + name, name});
  return q::encode_describe(server);
}

// `moved`: set to the valve and its new state by Open/Close, for the
// listener the hook calls once the model is unlocked.
Bytes respond(QtegraSimModel& m, const Bytes& tx, std::optional<std::pair<std::string, bool>>& moved) {
  const auto request = q::decode_request(tx);
  if (!request) return q::encode_error("Invalid Command");
  const auto& verb = request->verb;
  const auto& args = request->args;
  Bytes bad_arguments = q::encode_error("invalid number in " + verb);

  std::lock_guard lock(m.mutex);
  std::string line = verb;
  for (std::size_t i = 0; i < args.size(); ++i) line += (i == 0 ? " " : ",") + args[i];
  m.commands.push_back(std::move(line));
  if ((verb == "KeepAlive" || verb == "Ping") && args.empty()) return q::encode_ok();
  if (verb == "Describe" && args.empty()) {
    return m.describe_override.empty() ? describe(m) : q::encode_line(m.describe_override);
  }
  if (verb == "GetIntegrationTime" && args.empty()) return q::encode_number(m.integration_s);
  if (verb == "GetMagnetDAC" && args.empty()) return q::encode_number(m.dac);
  if (verb == "GetMagnetMoving" && args.empty()) {
    return q::encode_bool(m.clock != nullptr && m.clock->now() < m.moving_until);
  }
  if (verb == "SetMagnetDAC") {
    const auto dac = args.size() == 1 ? codec::parse_decimal(args[0]) : std::nullopt;
    if (!dac) return bad_arguments;
    m.dac = *dac;
    if (m.clock != nullptr) m.moving_until = m.clock->now() + m.move_time;
    return q::encode_ok();
  }
  if (verb == "BlankBeam") {
    const auto on = args.size() == 1 ? parse_switch(args[0], "true", "false") : std::nullopt;
    if (!on) return bad_arguments;
    m.blank = *on;
    return q::encode_ok();
  }
  if (verb == "ProtectDetector") {
    const auto on = args.size() == 2 ? parse_switch(args[1], "On", "Off") : std::nullopt;
    if (!on) return bad_arguments;
    m.protect[args[0]] = *on;
    return q::encode_ok();
  }
  if (verb == "SetDeflection" || verb == "SetGain") {
    const auto value = args.size() == 2 ? codec::parse_decimal(args[1]) : std::nullopt;
    if (!value) return bad_arguments;
    (verb == "SetDeflection" ? m.deflection : m.gain)[args[0]] = *value;
    return q::encode_ok();
  }
  if (verb == "GetDeflection" || verb == "GetGain") {
    if (args.size() != 1) return bad_arguments;
    return q::encode_number(value_or_zero(verb == "GetDeflection" ? m.deflection : m.gain, args[0]));
  }
  if (verb == "GetDeflections") {
    if (args.empty()) return bad_arguments;
    std::string values;
    for (const auto& name : args) values += (values.empty() ? "" : ",") + q::format_number(value_or_zero(m.deflection, name));
    return q::encode_line(values);
  }
  if (verb == "GetHighVoltage" && args.empty()) return q::encode_number(m.hv);
  if (verb == "SetHV" || verb == "SetIntegrationTime" || verb == "SetIonCounterVoltage") {
    const auto value = args.size() == 1 ? codec::parse_decimal(args[0]) : std::nullopt;
    if (!value) return bad_arguments;
    if (verb == "SetIonCounterVoltage") {
      m.params["CDD Supply Set"] = *value;
    } else {
      (verb == "SetHV" ? m.hv : m.integration_s) = *value;
    }
    return q::encode_ok();
  }
  if (verb == "SetParameter") {
    const auto value = args.size() == 2 ? codec::parse_decimal(args[1]) : std::nullopt;
    if (!value) return bad_arguments;
    m.params[args[0]] = *value;
    return q::encode_ok();
  }
  if (verb == "GetParameter") {
    if (args.size() != 1) return bad_arguments;
    if (m.parameter_source) {
      if (auto v = m.parameter_source(args[0])) return q::encode_number(*v);
    }
    return q::encode_number(parameter(m, args[0]));
  }
  if (verb == "GetParameters") {
    if (args.empty()) return bad_arguments;
    std::string values;
    for (const auto& name : args) values += (values.empty() ? "" : ",") + q::format_number(parameter(m, name));
    return q::encode_line(values);
  }
  if (verb == "ActivateCupConfiguration") {
    // "<cup> <sub_cup>": a space, which decode_request leaves in the argument.
    if (args.size() != 1 || args[0].find(' ') == std::string::npos) return bad_arguments;
    return q::encode_ok();
  }
  // A valve name may hold spaces but no comma: the whole argument is it.
  if (verb == "Open" || verb == "Close") {
    if (args.size() != 1) return bad_arguments;
    m.valves[args[0]] = verb == "Open";
    moved = {args[0], verb == "Open"};
    return q::encode_ok();
  }
  if (verb == "GetValveState") {
    if (args.size() != 1) return bad_arguments;
    const auto it = m.valves.find(args[0]);
    return q::encode_bool(it != m.valves.end() && it->second);
  }
  if (verb == "GetData" && args.empty()) {
    if (!m.data_override.empty()) return q::encode_line(m.data_override);
    const std::map<std::string, double> data = m.data_source ? m.data_source(m.integration_s) : m.intensities;
    if (data.empty()) return q::encode_error("no data");
    return q::encode_data(q::Pairs(data.begin(), data.end()));
  }
  return q::encode_error("Invalid Command");
}

}  // namespace

SimTransport::Hook qtegra_sim_hook(std::shared_ptr<QtegraSimModel> model) {
  return [model = std::move(model)](const Bytes& tx) {
    std::optional<std::pair<std::string, bool>> moved;
    Bytes reply = respond(*model, tx, moved);
    if (moved && model->on_valve) model->on_valve(moved->first, moved->second);
    if (model->on_command) {
      if (const auto request = q::decode_request(tx)) model->on_command(request->verb, request->args);
    }
    return reply;
  };
}

}  // namespace pychron::spectrometer
