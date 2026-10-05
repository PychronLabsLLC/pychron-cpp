#include "pychron/codecs/pychron_tx.hpp"

#include <string>

namespace pychron::codec::pychron_tx {

namespace {

std::string_view trim(std::string_view s) {
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

Result<Command> named(std::string_view verb, std::string_view name) {
  const auto n = trim(name);
  if (n.empty()) return fail(ErrorKind::Config, "pychron_tx: empty valve name");
  for (char c : n) {
    if (c == ',' || static_cast<unsigned char>(c) < 0x20) {
      return fail(ErrorKind::Config, "pychron_tx: valve name \"" + std::string(name) +
                                         "\" holds a comma or control character");
    }
  }
  return Command{to_bytes(std::string(verb) + " " + std::string(n) + std::string(kTerminator)), reply_spec()};
}

// An "ERROR <code> : <message>" reply as its error, or nullopt.
std::optional<Error> server_error(std::string_view text, const Bytes& reply) {
  if (!text.starts_with("ERROR")) return std::nullopt;
  auto rest = trim(text.substr(5));
  const auto colon = rest.find(':');
  const auto code = trim(rest.substr(0, colon));
  const auto message = colon == std::string_view::npos ? std::string_view{} : trim(rest.substr(colon + 1));
  ErrorKind kind = ErrorKind::Protocol;
  if (code == "012" || code == "014") kind = ErrorKind::Interlock;
  if (code == "003" || code == "004" || code == "005") kind = ErrorKind::Config;
  Error e = protocol_error("pychron_tx: the remote Pychron says ERROR " + std::string(code) + ": " +
                               std::string(message),
                           reply)
                .error();
  e.kind = kind;
  return e;
}

}  // namespace

const ReadSpec& reply_spec() noexcept {
  static const ReadSpec spec = ReadSpec::until_close();
  return spec;
}

Result<Command> open_valve(std::string_view name) { return named("Open", name); }
Result<Command> close_valve(std::string_view name) { return named("Close", name); }
Result<Command> get_valve_state(std::string_view name) { return named("GetValveState", name); }

Result<void> decode_actuation(const Bytes& reply) {
  const std::string whole = to_string(reply);
  const auto text = trim(whole);
  if (text == "OK" || text == "ok") return {};
  if (auto e = server_error(text, reply)) return fail(std::move(*e));
  return protocol_error("pychron_tx: expected OK", reply);
}

Result<bool> decode_state(const Bytes& reply) {
  const std::string whole = to_string(reply);
  const auto text = trim(whole);
  if (text == "OK" || text == "True") return true;
  if (text == "False") return false;
  if (auto e = server_error(text, reply)) return fail(std::move(*e));
  return protocol_error("pychron_tx: expected OK or False", reply);
}

Result<Request> decode_request(const Bytes& tx) {
  const std::string whole = to_string(tx);
  const auto text = trim(whole);
  if (text.empty()) return protocol_error("pychron_tx: empty command", tx);
  const auto space = text.find(' ');
  Request r;
  r.verb = std::string(text.substr(0, space));
  if (space != std::string_view::npos) r.name = std::string(trim(text.substr(space + 1)));
  return r;
}

Bytes encode_ok(bool changed) { return to_bytes(changed ? "OK" : "ok"); }
Bytes encode_state(bool open) { return to_bytes(open ? "OK" : "False"); }
Bytes encode_error(std::string_view code, std::string_view message) {
  return to_bytes("ERROR " + std::string(code) + " : " + std::string(message));
}

}  // namespace pychron::codec::pychron_tx
