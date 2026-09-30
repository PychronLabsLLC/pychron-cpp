#include "pychron/codecs/pulse_counter.hpp"

#include <limits>
#include <string>

namespace pychron::codec::pulse_counter {

namespace {

std::optional<std::uint64_t> parse_count(std::string_view s) {
  if (s.empty() || s.size() > 19) return std::nullopt;
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return v;
}

}  // namespace

Command read_counts() { return Command::ascii("R\r", ReadSpec::until(kTerminator)); }

Result<std::vector<std::uint64_t>> decode_counts(std::size_t channels, const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  if (text->starts_with("E")) return protocol_error("pulse_counter: counter error", reply);

  std::vector<std::uint64_t> counts;
  std::string_view rest = *text;
  while (true) {
    const auto comma = rest.find(',');
    auto count = parse_count(rest.substr(0, comma));
    if (!count) return protocol_error("pulse_counter: bad count", reply);
    counts.push_back(*count);
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  if (counts.size() != channels) {
    return protocol_error("pulse_counter: expected " + std::to_string(channels) + " channels, got " +
                              std::to_string(counts.size()),
                          reply);
  }
  return counts;
}

bool is_read_request(const Bytes& tx) { return tx == to_bytes("R\r"); }

Bytes encode_counts(const std::vector<std::uint64_t>& counts) {
  std::string text;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (i > 0) text += ',';
    text += std::to_string(counts[i]);
  }
  text += kTerminator;
  return to_bytes(text);
}

Bytes encode_error(std::string_view message) { return to_bytes("E " + std::string(message) + "\r"); }

}  // namespace pychron::codec::pulse_counter
