#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"

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

// Source and acquirer commands are added in Task 6.
Bytes respond(QtegraSimModel& m, const Bytes& tx) {
  const auto request = q::decode_request(tx);
  if (!request) return q::encode_error("empty command");
  const auto& verb = request->verb;
  const auto& args = request->args;
  const Bytes bad_arguments = q::encode_error("bad arguments for " + verb);

  std::lock_guard lock(m.mutex);
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
    const auto on = args.size() == 1 ? parse_switch(args[0], "True", "False") : std::nullopt;
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
  return q::encode_error("unknown command " + verb);
}

}  // namespace

SimTransport::Hook qtegra_sim_hook(std::shared_ptr<QtegraSimModel> model) {
  return [model = std::move(model)](const Bytes& tx) { return respond(*model, tx); };
}

}  // namespace pychron::spectrometer
