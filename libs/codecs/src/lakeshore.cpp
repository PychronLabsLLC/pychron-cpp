#include "pychron/codecs/lakeshore.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <vector>

namespace pychron::codec::lakeshore {

namespace {

std::string_view trim(std::string_view s) {
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

Command line(std::string text, bool reply) {
  Bytes tx = to_bytes(text + std::string(kTerminator));
  return reply ? Command{std::move(tx), reply_spec()} : Command::write_only(std::move(tx));
}

Result<void> check_output(int output) {
  if (output < 1 || output > 4)
    return fail(ErrorKind::Config, "lakeshore: output " + std::to_string(output) + " is outside 1..4");
  return {};
}

// Fixed three decimals, never the process locale.
std::string fixed3(double v) {
  std::array<char, 32> buf{};
  auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, 3);
  return std::string(buf.data(), r.ptr);
}

}  // namespace

const ReadSpec& reply_spec() noexcept {
  static const ReadSpec spec = ReadSpec::until("\n");
  return spec;
}

Command identify() { return line("*IDN?", true); }
Command clear_status() { return line("*CLS", false); }

Result<Command> read_input(char input, Units units) {
  const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(input)));
  if (upper < 'A' || upper > 'D')
    return fail(ErrorKind::Config, std::string("lakeshore: input '") + input + "' is not A..D");
  return line(std::string(units == Units::Kelvin ? "KRDG? " : "CRDG? ") + upper, true);
}

Result<Command> set_setpoint(int output, double value) {
  if (auto ok = check_output(output); !ok) return fail(std::move(ok).error());
  if (!std::isfinite(value) || value < 0 || value > 2000)
    return fail(ErrorKind::Config, "lakeshore: setpoint " + fixed3(value) + " is outside 0..2000");
  return line("SETP " + std::to_string(output) + "," + fixed3(value), false);
}

Result<Command> query_setpoint(int output) {
  if (auto ok = check_output(output); !ok) return fail(std::move(ok).error());
  return line("SETP? " + std::to_string(output), true);
}

Result<Command> set_range(int output, int range) {
  if (auto ok = check_output(output); !ok) return fail(std::move(ok).error());
  if (range < 0 || range > 5)
    return fail(ErrorKind::Config, "lakeshore: heater range " + std::to_string(range) + " is outside 0..5");
  return line("RANGE " + std::to_string(output) + "," + std::to_string(range), false);
}

Result<Command> query_range(int output) {
  if (auto ok = check_output(output); !ok) return fail(std::move(ok).error());
  return line("RANGE? " + std::to_string(output), true);
}

Result<double> decode_number(const Bytes& reply) {
  const std::string whole = to_string(reply);
  auto v = parse_decimal(trim(whole));
  if (!v) return protocol_error("lakeshore: not a number", reply);
  return *v;
}

Result<int> decode_range(const Bytes& reply) {
  const std::string whole = to_string(reply);
  const auto t = trim(whole);
  if (t.size() != 1 || t[0] < '0' || t[0] > '5') return protocol_error("lakeshore: not a heater range", reply);
  return t[0] - '0';
}

Result<Identity> decode_identity(const Bytes& reply) {
  const std::string whole = to_string(reply);
  std::vector<std::string> fields;
  std::string_view rest = trim(whole);
  for (;;) {
    const auto at = rest.find(',');
    fields.emplace_back(trim(rest.substr(0, at)));
    if (at == std::string_view::npos) break;
    rest.remove_prefix(at + 1);
  }
  if (fields.size() != 4 || fields[0].empty() || fields[1].empty())
    return protocol_error("lakeshore: identity is not four fields", reply);
  return Identity{fields[0], fields[1], fields[2], fields[3]};
}

Result<Request> decode_request(const Bytes& tx) {
  const std::string whole = to_string(tx);
  const auto t = trim(whole);
  if (t.empty()) return protocol_error("lakeshore: empty command", tx);
  const auto space = t.find(' ');
  Request r;
  for (char c : t.substr(0, space)) r.header += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (space != std::string_view::npos) r.argument = std::string(trim(t.substr(space + 1)));
  return r;
}

Bytes encode_number(double value) {
  std::string s = fixed3(std::fabs(value));
  while (s.find('.') < 3) s.insert(0, "0");  // "+077.123", as the unit pads
  return to_bytes((value < 0 ? "-" : "+") + s + "\r\n");
}

Bytes encode_line(std::string_view text) { return to_bytes(std::string(text) + "\r\n"); }

}  // namespace pychron::codec::lakeshore
