#include "pychron/codecs/microion.hpp"

#include <cstdio>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

namespace pychron::codec::microion {

namespace {

constexpr std::string_view kOk = "PROGM OK";

const ReadSpec& line_reply() {
  static const ReadSpec spec = ReadSpec::until(kTerminator);
  return spec;
}

bool valid_address(int address) { return address >= kMinAddress && address <= kMaxAddress; }

Unexpected<Error> address_error(int address) {
  return fail(ErrorKind::Config, "micro-ion address " + std::to_string(address) + " out of range 0..255");
}

std::string format_address(int address) {
  char buf[3];
  std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned>(address));
  return buf;
}

std::optional<int> hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return std::nullopt;
}

std::optional<int> parse_address(std::string_view s) {
  if (s.size() != 2) return std::nullopt;
  auto hi = hex_digit(s[0]);
  auto lo = hex_digit(s[1]);
  if (!hi || !lo) return std::nullopt;
  return *hi * 16 + *lo;
}

std::optional<Sensor> sensor_named(std::string_view name) {
  for (Sensor s : {Sensor::IonGauge, Sensor::ConvectronA, Sensor::ConvectronB}) {
    if (to_string(s) == name) return s;
  }
  return std::nullopt;
}

Command command(int address, std::string_view body) {
  return Command::ascii("#" + format_address(address) + std::string(body) + std::string(kTerminator), line_reply());
}

// The payload of "*<aa> <payload>\r" from `address`. "?<aa> <message>" and
// anything else malformed are Protocol errors.
Result<std::string> payload(int address, const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  std::string_view line = *text;
  if (line.starts_with('\n')) line.remove_prefix(1);

  if (line.size() < 4 || (line[0] != '*' && line[0] != '?') || line[3] != ' ') {
    return protocol_error("expected \"*<address> <data>\"", reply);
  }
  auto from = parse_address(line.substr(1, 2));
  if (!from) return protocol_error("invalid address", reply);
  if (*from != address) {
    return protocol_error("reply from address " + format_address(*from) + ", expected " + format_address(address),
                          reply);
  }
  std::string_view data = line.substr(4);
  if (line[0] == '?') return protocol_error("controller error \"" + std::string(data) + "\"", reply);
  return std::string(data);
}

// "d.ddE+dd" regardless of the process locale.
std::string format_pressure(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::uppercase << std::scientific << std::setprecision(2) << value;
  return out.str();
}

}  // namespace

std::string_view to_string(Sensor sensor) noexcept {
  switch (sensor) {
    case Sensor::IonGauge: return "IG";
    case Sensor::ConvectronA: return "CG1";
    case Sensor::ConvectronB: return "CG2";
  }
  return "unknown";
}

// --- host side --------------------------------------------------------------

Result<Command> read_pressure(int address, int channel) {
  if (!valid_address(address)) return address_error(address);
  if (channel < kFirstChannel || channel > kLastChannel) {
    return fail(ErrorKind::Config, "micro-ion channel " + std::to_string(channel) + " out of range " +
                                       std::to_string(kFirstChannel) + ".." + std::to_string(kLastChannel));
  }
  return command(address, "DS " + std::string(to_string(static_cast<Sensor>(channel))));
}

Result<Command> set_ion_gauge(int address, bool on) {
  if (!valid_address(address)) return address_error(address);
  return command(address, on ? "IG1 ON" : "IG1 OFF");
}

Result<double> decode_pressure(int address, const Bytes& reply) {
  auto data = payload(address, reply);
  if (!data) return fail(std::move(data).error());
  auto v = parse_decimal(*data);
  if (!v || *v < 0.0) return protocol_error("invalid pressure \"" + *data + "\"", reply);
  if (*v >= kGaugeOff) return protocol_error("gauge off or not connected", reply);
  return *v;
}

Result<void> decode_ok(int address, const Bytes& reply) {
  auto data = payload(address, reply);
  if (!data) return fail(std::move(data).error());
  if (*data != kOk) return protocol_error("expected \"" + std::string(kOk) + "\"", reply);
  return {};
}

// --- controller side -----------------------------------------------------------

Result<Request> decode_request(const Bytes& tx) {
  auto text = strip_terminator(tx, kTerminator);
  if (!text) return fail(std::move(text).error());
  std::string_view line = *text;
  if (line.size() < 3 || line[0] != '#') return protocol_error("expected \"#<address><command>\"", tx);
  auto address = parse_address(line.substr(1, 2));
  if (!address) return protocol_error("invalid address", tx);

  std::string_view body = line.substr(3);
  if (body == "IG1 ON") return Request{Request::Kind::IonGaugeOn, *address, 0};
  if (body == "IG1 OFF") return Request{Request::Kind::IonGaugeOff, *address, 0};
  if (body.starts_with("DS ")) {
    if (auto sensor = sensor_named(body.substr(3))) {
      return Request{Request::Kind::Pressure, *address, static_cast<int>(*sensor)};
    }
  }
  return protocol_error("unknown command", tx);
}

std::optional<int> addressee(const Bytes& tx) {
  if (tx.size() < 3 || tx[0] != '#') return std::nullopt;
  return parse_address(pychron::to_string(Bytes(tx.begin() + 1, tx.begin() + 3)));
}

Bytes encode_pressure(int address, double value) {
  return to_bytes("*" + format_address(address) + " " + format_pressure(value) + std::string(kTerminator));
}

Bytes encode_ok(int address) {
  return to_bytes("*" + format_address(address) + " " + std::string(kOk) + std::string(kTerminator));
}

Bytes encode_error(int address, std::string_view message) {
  return to_bytes("?" + format_address(address) + " " + std::string(message) + std::string(kTerminator));
}

}  // namespace pychron::codec::microion
