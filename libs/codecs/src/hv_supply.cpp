#include "pychron/codecs/hv_supply.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace pychron::codec::hv_supply {

namespace {

// One decimal place, locale independent.
std::string format_volts(double volts) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.1f", volts);
  std::string s(buf);
  for (auto& c : s) {
    if (c == ',') c = '.';
  }
  return s;
}

ReadSpec line() { return ReadSpec::until(kTerminator); }

}  // namespace

Result<Command> set_voltage(double volts) {
  if (!std::isfinite(volts) || volts < 0.0) {
    return fail(ErrorKind::Config, "hv_supply: " + std::to_string(volts) + " V is not a valid setpoint");
  }
  return Command::ascii("VSET " + format_volts(volts) + "\r", line());
}

Command read_setpoint() { return Command::ascii("VSET?\r", line()); }

Command read_output() { return Command::ascii("VOUT?\r", line()); }

Result<void> decode_ok(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  if (*text != "OK") return protocol_error("hv_supply: command rejected", reply);
  return {};
}

Result<double> decode_voltage(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  auto v = parse_decimal(*text);
  if (!v) return protocol_error("hv_supply: bad voltage", reply);
  return *v;
}

Result<Request> decode_request(const Bytes& tx) {
  auto text = strip_terminator(tx, kTerminator);
  if (!text) return fail(std::move(text).error());
  if (*text == "VSET?") return Request{Request::Kind::ReadSetpoint, 0.0};
  if (*text == "VOUT?") return Request{Request::Kind::ReadOutput, 0.0};
  if (text->starts_with("VSET ")) {
    auto v = parse_decimal(std::string_view(*text).substr(5));
    if (!v || *v < 0.0) return protocol_error("hv_supply: bad setpoint", tx);
    return Request{Request::Kind::Set, *v};
  }
  return protocol_error("hv_supply: unknown command", tx);
}

Bytes encode_ok() { return to_bytes("OK\r"); }

Bytes encode_voltage(double volts) { return to_bytes(format_volts(volts) + "\r"); }

Bytes encode_error(std::string_view message) { return to_bytes("ERR " + std::string(message) + "\r"); }

}  // namespace pychron::codec::hv_supply
