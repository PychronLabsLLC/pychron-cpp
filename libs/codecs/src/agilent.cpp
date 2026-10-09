#include "pychron/codecs/agilent.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace pychron::codec::agilent {

namespace {

std::string_view trim(std::string_view s) {
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

std::string upper(std::string_view s) {
  std::string out;
  for (char c : s) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  for (;;) {
    const auto at = s.find(sep);
    out.push_back(s.substr(0, at));
    if (at == std::string_view::npos) return out;
    s.remove_prefix(at + 1);
  }
}

Command line(std::string_view text, bool reply) {
  Bytes tx = to_bytes(std::string(text) + std::string(kTerminator));
  return reply ? Command{std::move(tx), reply_spec()} : Command::write_only(std::move(tx));
}

Result<Command> routed(std::string_view header, std::string_view address, bool reply) {
  auto ch = channel(address);
  if (!ch) return fail(std::move(ch).error());
  return line(std::string(header) + " (@" + *ch + ")", reply);
}

std::string reply_text(const Bytes& reply) { return std::string(trim(to_string(reply))); }

}  // namespace

const ReadSpec& reply_spec() noexcept {
  static const ReadSpec spec = ReadSpec::until("\n");
  return spec;
}

Result<std::string> channel(std::string_view address) {
  const auto a = trim(address);
  const bool ok = a.size() == 3 && a[0] >= '1' && a[0] <= '3' && std::isdigit(static_cast<unsigned char>(a[1])) &&
                  std::isdigit(static_cast<unsigned char>(a[2])) && !(a[1] == '0' && a[2] == '0');
  if (!ok) {
    return fail(ErrorKind::Config, "agilent: channel \"" + std::string(address) +
                                       R"(" is not slot 1..3 then channel 01..99 (e.g. "101"))");
  }
  return std::string(a);
}

Command identify() { return line("*IDN?", true); }
Command clear_status() { return line("*CLS", false); }
Command next_error() { return line("SYST:ERR?", true); }
Result<Command> route_open(std::string_view address) { return routed("ROUT:OPEN", address, false); }
Result<Command> route_close(std::string_view address) { return routed("ROUT:CLOSE", address, false); }
Result<Command> query_open(std::string_view address) { return routed("ROUT:OPEN?", address, true); }
Result<Command> query_close(std::string_view address) { return routed("ROUT:CLOSE?", address, true); }

Result<Identity> decode_identity(const Bytes& reply) {
  const auto text = reply_text(reply);
  const auto fields = split(text, ',');
  if (fields.size() != 4) return protocol_error("agilent: identity is not four fields", reply);
  Identity id{std::string(trim(fields[0])), std::string(trim(fields[1])), std::string(trim(fields[2])),
              std::string(trim(fields[3]))};
  if (id.manufacturer.empty() || id.model.empty()) return protocol_error("agilent: identity is empty", reply);
  return id;
}

bool is_switch_unit(const Identity& identity) noexcept {
  const auto maker = upper(identity.manufacturer);
  const bool known_maker = maker.find("AGILENT") != std::string::npos ||
                           maker.find("KEYSIGHT") != std::string::npos ||
                           maker.find("HEWLETT") != std::string::npos;
  const auto model = upper(identity.model);
  const bool known_model = model == "34970A" || model == "34972A" || model == "DAQ970A" || model == "DAQ973A";
  return known_maker && known_model;
}

Result<bool> decode_route_state(const Bytes& reply) {
  const auto text = reply_text(reply);
  if (text == "1") return true;
  if (text == "0") return false;
  return protocol_error("agilent: route state is not 0 or 1", reply);
}

Result<std::optional<InstrumentError>> decode_error(const Bytes& reply) {
  const auto text = reply_text(reply);
  const auto comma = text.find(',');
  if (comma == std::string::npos) return protocol_error("agilent: error reply has no comma", reply);
  const auto code_text = trim(std::string_view(text).substr(0, comma));
  auto message = trim(std::string_view(text).substr(comma + 1));
  if (message.size() < 2 || message.front() != '"' || message.back() != '"') {
    return protocol_error("agilent: error message is not quoted", reply);
  }
  message = message.substr(1, message.size() - 2);
  std::string_view digits = code_text;
  bool negative = false;
  if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
    negative = digits.front() == '-';
    digits.remove_prefix(1);
  }
  if (digits.empty() || digits.size() > 6 ||
      !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
    return protocol_error("agilent: error code is not a number", reply);
  }
  int code = 0;
  for (char c : digits) code = code * 10 + (c - '0');
  if (negative) code = -code;
  if (code == 0) return std::optional<InstrumentError>{};
  return std::optional<InstrumentError>{InstrumentError{code, std::string(message)}};
}

Result<Request> decode_request(const Bytes& tx) {
  const std::string whole = to_string(tx);
  const auto text = trim(whole);
  if (text.empty()) return protocol_error("agilent: empty command", tx);
  const auto space = text.find(' ');
  Request r;
  r.header = std::string(text.substr(0, space));
  if (space != std::string_view::npos) r.argument = std::string(trim(text.substr(space + 1)));
  return r;
}

std::optional<std::string> single_channel(std::string_view argument) {
  const auto a = trim(argument);
  if (a.size() < 4 || !a.starts_with("(@") || a.back() != ')') return std::nullopt;
  auto ch = channel(a.substr(2, a.size() - 3));
  if (!ch) return std::nullopt;
  return *ch;
}

Bytes encode_line(std::string_view text) { return to_bytes(std::string(text) + "\n"); }

Bytes encode_error(int code, std::string_view message) {
  if (code == 0) return encode_line("+0,\"No error\"");
  return encode_line(std::to_string(code) + ",\"" + std::string(message) + "\"");
}

}  // namespace pychron::codec::agilent
