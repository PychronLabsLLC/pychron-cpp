#include "pychron/codecs/varian_xgs600.hpp"

#include <cctype>
#include <cstdio>

namespace pychron::codec::varian_xgs600 {

namespace {

std::string_view trim(std::string_view s) {
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

}  // namespace

const ReadSpec& reply_spec() noexcept {
  static const ReadSpec spec = ReadSpec::until("\r");
  return spec;
}

Result<void> validate_address(std::string_view address) {
  const bool ok = address.size() == 2 && std::isxdigit(static_cast<unsigned char>(address[0])) &&
                  std::isxdigit(static_cast<unsigned char>(address[1]));
  if (!ok) return fail(ErrorKind::Config, "xgs600: address \"" + std::string(address) + "\" is not two hex digits");
  return {};
}

Result<void> validate_label(std::string_view label) {
  bool ok = !label.empty() && label.size() <= 8;
  for (char c : label) ok = ok && std::isalnum(static_cast<unsigned char>(c));
  if (!ok) return fail(ErrorKind::Config, "xgs600: label \"" + std::string(label) + "\" is not 1..8 letters and digits");
  return {};
}

Result<Command> read_pressure(std::string_view address, std::string_view label) {
  if (auto ok = validate_address(address); !ok) return fail(std::move(ok).error());
  if (auto ok = validate_label(label); !ok) return fail(std::move(ok).error());
  return Command{to_bytes("#" + std::string(address) + "02U" + std::string(label) + std::string(kTerminator)),
                 reply_spec()};
}

Result<double> decode_pressure(const Bytes& reply) {
  const std::string whole = to_string(reply);
  const auto text = trim(whole);
  if (text.starts_with("?")) return protocol_error("xgs600: the controller rejected the command", reply);
  if (!text.starts_with(">")) return protocol_error("xgs600: expected '>'", reply);
  const auto body = trim(text.substr(1));
  if (body == "OFF") return protocol_error("xgs600: gauge off", reply);
  auto value = parse_decimal(body);
  if (!value || *value < 0) return protocol_error("xgs600: not a pressure", reply);
  return *value;
}

std::optional<std::string> requested_label(std::string_view address, const Bytes& tx) {
  const std::string whole = to_string(tx);
  const auto text = trim(whole);
  const std::string prefix = "#" + std::string(address) + "02U";
  if (!text.starts_with(prefix)) return std::nullopt;
  const auto label = text.substr(prefix.size());
  if (!validate_label(label)) return std::nullopt;
  return std::string(label);
}

Bytes encode_pressure(double value) {
  char buf[32];
  std::snprintf(buf, sizeof buf, ">%.3E\r", value);
  return to_bytes(buf);
}

Bytes encode_off() { return to_bytes(">OFF\r"); }
Bytes encode_rejected() { return to_bytes("?FF\r"); }

}  // namespace pychron::codec::varian_xgs600
