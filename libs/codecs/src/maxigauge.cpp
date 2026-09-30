#include "pychron/codecs/maxigauge.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

namespace pychron::codec::maxigauge {

namespace {

const ReadSpec& line_reply() {
  static const ReadSpec spec = ReadSpec::until(kTerminator);
  return spec;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// [+-]digits[.digits][(E|e)[+-]digits], at least one mantissa digit. The
// grammar is checked here so parsing never depends on the process locale.
bool is_decimal_number(std::string_view s) {
  std::size_t i = 0;
  auto digits = [&] {
    std::size_t start = i;
    while (i < s.size() && is_digit(s[i])) ++i;
    return i - start;
  };
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t mantissa = digits();
  if (i < s.size() && s[i] == '.') {
    ++i;
    mantissa += digits();
  }
  if (mantissa == 0) return false;
  if (i < s.size() && (s[i] == 'E' || s[i] == 'e')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (digits() == 0) return false;
  }
  return i == s.size();
}

std::optional<double> parse_number(std::string_view s) {
  if (!is_decimal_number(s)) return std::nullopt;
  std::istringstream in{std::string(s)};
  in.imbue(std::locale::classic());
  double v = 0.0;
  in >> v;
  if (in.fail() || !std::isfinite(v)) return std::nullopt;
  return v;
}

std::optional<Status> parse_status(std::string_view s) {
  if (s.size() != 1 || s[0] < '0' || s[0] > '6') return std::nullopt;
  return static_cast<Status>(s[0] - '0');
}

// Splits "a,b,c" on commas; empty fields are kept.
std::vector<std::string_view> split_fields(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (true) {
    std::size_t comma = s.find(',', start);
    out.push_back(s.substr(start, comma - start));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}

Result<Reading> parse_pair(std::string_view status, std::string_view value, const Bytes& reply) {
  auto st = parse_status(status);
  if (!st) return protocol_error("invalid status \"" + std::string(status) + "\"", reply);
  auto v = parse_number(value);
  if (!v) return protocol_error("invalid pressure \"" + std::string(value) + "\"", reply);
  return Reading{*st, *v};
}

std::string format_reading(const Reading& r) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << static_cast<int>(r.status) << ',' << std::showpos << std::uppercase << std::scientific
      << std::setprecision(4) << r.value;
  return out.str();
}

}  // namespace

std::string_view to_string(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::Underrange: return "underrange";
    case Status::Overrange: return "overrange";
    case Status::SensorError: return "sensor error";
    case Status::SensorOff: return "sensor off";
    case Status::NoSensor: return "no sensor";
    case Status::IdentificationError: return "identification error";
  }
  return "unknown";
}

std::string_view to_string(Units units) noexcept {
  switch (units) {
    case Units::Mbar: return "mbar";
    case Units::Torr: return "torr";
    case Units::Pascal: return "pa";
  }
  return "unknown";
}

// --- host side --------------------------------------------------------------

Result<Command> read_pressure(int channel) {
  if (channel < kFirstChannel || channel > kLastChannel) {
    return fail(ErrorKind::Config, "maxigauge channel " + std::to_string(channel) + " out of range " +
                                       std::to_string(kFirstChannel) + ".." + std::to_string(kLastChannel));
  }
  return Command::ascii("PR" + std::to_string(channel) + std::string(kTerminator), line_reply());
}

Command read_all_pressures() { return Command::ascii("PRX" + std::string(kTerminator), line_reply()); }

Command read_units() { return Command::ascii("UNI" + std::string(kTerminator), line_reply()); }

Command enquiry() { return Command{Bytes{kEnq}, line_reply()}; }

Result<void> decode_ack(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  if (text->size() == 1 && static_cast<std::uint8_t>((*text)[0]) == kAck) return {};
  if (text->size() == 1 && static_cast<std::uint8_t>((*text)[0]) == kNak) {
    return protocol_error("NAK: gauge rejected the command", reply);
  }
  return protocol_error("expected ACK", reply);
}

Result<Reading> decode_reading(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  auto fields = split_fields(*text);
  if (fields.size() != 2) return protocol_error("expected \"<status>,<value>\"", reply);
  return parse_pair(fields[0], fields[1], reply);
}

Result<double> pressure_value(const Reading& reading) {
  switch (reading.status) {
    case Status::Ok:
    case Status::Underrange:
    case Status::Overrange:
      return reading.value;
    default:
      return protocol_error("gauge reports " + std::string(to_string(reading.status)));
  }
}

Result<double> decode_pressure(const Bytes& reply) {
  auto r = decode_reading(reply);
  if (!r) return fail(std::move(r).error());
  auto v = pressure_value(*r);
  if (!v) return protocol_error(v.error().what, reply);
  return v;
}

Result<std::vector<Reading>> decode_all_readings(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  auto fields = split_fields(*text);
  if (fields.size() != 2 * kChannelCount) {
    return protocol_error("expected " + std::to_string(kChannelCount) + " \"<status>,<value>\" pairs", reply);
  }
  std::vector<Reading> out;
  out.reserve(kChannelCount);
  for (std::size_t i = 0; i < fields.size(); i += 2) {
    auto r = parse_pair(fields[i], fields[i + 1], reply);
    if (!r) return fail(std::move(r).error());
    out.push_back(*r);
  }
  return out;
}

Result<Units> decode_units(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  if (text->size() != 1 || (*text)[0] < '0' || (*text)[0] > '2') {
    return protocol_error("invalid unit code", reply);
  }
  return static_cast<Units>((*text)[0] - '0');
}

// --- gauge side ---------------------------------------------------------------

Result<Request> decode_request(const Bytes& tx) {
  if (tx.size() == 1 && tx[0] == kEnq) return Request{Request::Kind::Enquiry, 0};

  std::string text = pychron::to_string(tx);
  if (text.ends_with("\r\n")) {
    text.resize(text.size() - 2);
  } else if (text.ends_with("\r")) {
    text.resize(text.size() - 1);
  } else {
    return protocol_error("unterminated mnemonic", tx);
  }

  if (text == "PRX") return Request{Request::Kind::AllPressures, 0};
  if (text == "UNI") return Request{Request::Kind::Units, 0};
  if (text.size() == 3 && text.starts_with("PR") && is_digit(text[2])) {
    int channel = text[2] - '0';
    if (channel >= kFirstChannel && channel <= kLastChannel) return Request{Request::Kind::Pressure, channel};
  }
  return protocol_error("unknown mnemonic", tx);
}

Bytes encode_ack() { return Bytes{kAck, '\r', '\n'}; }

Bytes encode_nak() { return Bytes{kNak, '\r', '\n'}; }

Bytes encode_reading(const Reading& reading) {
  return to_bytes(format_reading(reading) + std::string(kTerminator));
}

Bytes encode_all_readings(std::span<const Reading> readings) {
  std::string line;
  for (const auto& r : readings) {
    if (!line.empty()) line += ',';
    line += format_reading(r);
  }
  return to_bytes(line + std::string(kTerminator));
}

Bytes encode_units(Units units) {
  return to_bytes(std::to_string(static_cast<int>(units)) + std::string(kTerminator));
}

}  // namespace pychron::codec::maxigauge
