#include "pychron/codecs/isotopx_ngx.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace pychron::codec::ngx {

namespace {

const ReadSpec& line_reply() {
  static const ReadSpec spec = ReadSpec::until(kLineEnd);
  return spec;
}

Command cmd(std::string_view body, std::string_view send_terminator) {
  std::string tx(body);
  tx += send_terminator;
  return Command::ascii(tx, line_reply());
}

// Free of the characters that delimit NGX fields and lines.
bool clean(std::string_view s) {
  return !s.empty() && std::none_of(s.begin(), s.end(), [](char c) {
    return c == ',' || c == '#' || c == '\n' || c == '\r';
  });
}

bool token_ok(std::string_view s) {
  return clean(s) && std::none_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t'; });
}

bool vendor_name_ok(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '.' || c == '+' || c == '-';
  });
}

std::string_view trim(std::string_view s) {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && ws(s.back())) s.remove_suffix(1);
  return s;
}

// IX SOURCE_CONTROL_PARAMETERS, in Param order.
struct MapEntry { Param p; std::string_view mnemonic; std::string_view name; };
constexpr std::array<MapEntry, 15> kMap{{
    {Param::IonEnergy, "IE", "IonEnergy"},
    {Param::YFocus, "YF", "YFocus"},
    {Param::YBias, "YB", "YBias"},
    {Param::ZFocus, "ZF", "ZFocus"},
    {Param::ZBias, "ZB", "ZBias"},
    {Param::ElectronEnergy, "EE", "ElectronEnergy"},
    {Param::IonRepeller, "IR", "IonRepeller"},
    {Param::TrapVoltage, "TV", "TrapVoltage"},
    {Param::FilamentCurrent, "FC", "FilamentCurrent"},
    {Param::FilamentVoltage, "FV", "FilamentVoltage"},
    {Param::TrapCurrent, "TC", "TrapCurrent"},
    {Param::EmissionCurrent, "EC", "EmissionCurrent"},
    {Param::ConfinementVoltage, "CV", "ConfinementVoltage"},
    {Param::ESAPlus, "ESA+", "ESA+Plate"},
    {Param::ESAMinus, "ESA-", "ESA-Plate"},
}};

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    auto i = s.find(sep, start);
    if (i == std::string_view::npos) { out.push_back(s.substr(start)); return out; }
    out.push_back(s.substr(start, i - start));
    start = i + 1;
  }
}

}  // namespace

Result<std::string> strip_line(const Bytes& line) {
  std::string text = to_string(line);
  if (text.empty() || text.back() != '\n') return protocol_error("missing line terminator", line);
  text.pop_back();
  if (!text.empty() && text.back() == '\r') text.pop_back();
  if (!text.empty() && text.back() == '#') text.pop_back();
  return text;
}

namespace {

// "Exx" with two digits.
std::optional<int> e_code(std::string_view head) {
  if (head.size() == 3 && head[0] == 'E' && head[1] >= '0' && head[1] <= '9' && head[2] >= '0' &&
      head[2] <= '9') {
    return (head[1] - '0') * 10 + (head[2] - '0');
  }
  return std::nullopt;
}

// Reply body without terminator, or the device error it reports. A bare
// "Exx" (IX ERRORS) or "Exx,text" with xx != 00 is an error.
Result<std::string> reply_text(const Bytes& reply) {
  auto text = strip_line(reply);
  if (!text) return fail(std::move(text).error());
  std::string_view body = trim(*text);
  auto comma = body.find(',');
  auto code = e_code(body.substr(0, comma));
  if (code && *code != 0) {
    std::string what = "NGX error E" + std::string(body.substr(1, 2)) + " (" +
                       std::string(error_text(*code)) + ")";
    if (comma != std::string_view::npos) what += ": " + std::string(body.substr(comma + 1));
    return fail(error_kind(*code), what + ": reply \"" + escape(reply) + "\"");
  }
  return std::string(body);
}

Result<double> number(std::string_view s, const Bytes& reply) {
  auto v = parse_decimal(trim(s));
  if (!v) return protocol_error("unparsable number \"" + std::string(s) + "\"", reply);
  return *v;
}

// 1-2 digits in [0, max] (Python strptime %H/%M/%S).
std::optional<int> clock_field(std::string_view s, int max) {
  if (s.empty() || s.size() > 2) return std::nullopt;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
  }
  if (v > max) return std::nullopt;
  return v;
}

// "%H:%M:%S.%f": %f is 1-6 digits, right-padded to microseconds.
std::optional<ClockTime> parse_clock(std::string_view s) {
  auto dot = s.find('.');
  if (dot == std::string_view::npos) return std::nullopt;
  auto hms = split(s.substr(0, dot), ':');
  if (hms.size() != 3) return std::nullopt;
  auto h = clock_field(hms[0], 23);
  auto m = clock_field(hms[1], 59);
  auto sec = clock_field(hms[2], 61);  // strptime allows leap seconds
  std::string_view frac = s.substr(dot + 1);
  if (!h || !m || !sec || frac.empty() || frac.size() > 6) return std::nullopt;
  int us = 0;
  for (std::size_t i = 0; i < 6; ++i) {
    int d = 0;
    if (i < frac.size()) {
      if (frac[i] < '0' || frac[i] > '9') return std::nullopt;
      d = frac[i] - '0';
    }
    us = us * 10 + d;
  }
  return ClockTime{*h, *m, *sec, us};
}

Result<Command> named(std::string_view verb, std::string_view arg, std::string_view what,
                      std::string_view send_terminator) {
  if (!token_ok(arg)) return fail(ErrorKind::Config, "invalid NGX " + std::string(what));
  return cmd(std::string(verb) + " " + std::string(arg), send_terminator);
}

}  // namespace

std::string format_float(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  // Shortest round-trip digits; then lay them out the way Python's float
  // repr does (fixed for -4 <= exp < 16, else scientific, 2+ exponent digits).
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
  std::string_view sci(buf, static_cast<std::size_t>(res.ptr - buf));
  bool neg = !sci.empty() && sci[0] == '-';
  if (neg) sci.remove_prefix(1);
  auto e = sci.find('e');
  std::string digits;
  for (char c : sci.substr(0, e)) if (c != '.') digits += c;
  // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): the exponent to_chars wrote just above
  int exp = std::atoi(std::string(sci.substr(e + 1)).c_str());

  std::string out = neg ? "-" : "";
  int n = static_cast<int>(digits.size());
  if (exp >= -4 && exp < 16) {
    int point = exp + 1;
    if (point <= 0) {
      out += "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
    } else if (point >= n) {
      out += digits + std::string(static_cast<std::size_t>(point - n), '0') + ".0";
    } else {
      out += digits.substr(0, point) + "." + digits.substr(point);
    }
  } else {
    out += digits.substr(0, 1);
    if (n > 1) out += "." + digits.substr(1);
    out += exp < 0 ? "e-" : "e+";
    int a = std::abs(exp);
    if (a < 10) out += '0';
    out += std::to_string(a);
  }
  return out;
}

Result<Command> login(std::string_view user, std::string_view password,
                      std::string_view send_terminator) {
  if (!clean(user) || !clean(password)) return fail(ErrorKind::Config, "invalid NGX login credentials");
  return cmd("Login " + std::string(user) + "," + std::string(password), send_terminator);
}

Command get_mass(std::string_view send_terminator) { return cmd("GETMASS", send_terminator); }

Result<int> settling_delay_ms(double settling_time_s) {
  if (!std::isfinite(settling_time_s) || settling_time_s < 0 || settling_time_s > 2.0e6)
    return fail(ErrorKind::Config, "invalid NGX settling time");
  return static_cast<int>(std::trunc(settling_time_s * 1000));
}

Result<Command> set_mass(double mass, int delay_ms, bool deflect, std::string_view send_terminator) {
  if (!std::isfinite(mass) || mass < 0) return fail(ErrorKind::Config, "invalid NGX mass");
  if (delay_ms < 0) return fail(ErrorKind::Config, "invalid NGX settling delay");
  std::string body = "SetMass " + format_float(mass) + "," + std::to_string(delay_ms);
  if (deflect) body += ",deflect";
  return cmd(body, send_terminator);
}

Result<Command> start_acq(double integration_time_s, std::string_view rcs_id,
                          std::string_view send_terminator) {
  if (!std::isfinite(integration_time_s) || integration_time_s >= 2.0e9)
    return fail(ErrorKind::Config, "invalid NGX integration time");
  auto count = static_cast<long long>(std::trunc(integration_time_s));
  if (count < 1) return fail(ErrorKind::Config, "NGX StartAcq integration time must be >= 1 s");
  if (!token_ok(rcs_id)) return fail(ErrorKind::Config, "invalid NGX rcs id");
  return cmd("StartAcq " + std::to_string(count) + "," + std::string(rcs_id), send_terminator);
}

Command stop_acq(std::string_view send_terminator) { return cmd("StopAcq", send_terminator); }

Result<Command> set_acq_period(int period_ms, std::string_view send_terminator) {
  if (period_ms < 1) return fail(ErrorKind::Config, "NGX acquisition period must be >= 1 ms");
  return cmd("SetAcqPeriod " + std::to_string(period_ms), send_terminator);
}

Command sab(bool enabled, std::string_view send_terminator) {
  return cmd(enabled ? "SAB 1" : "SAB 0", send_terminator);
}

std::string_view mnemonic(Param p) noexcept {
  for (auto& e : kMap) if (e.p == p) return e.mnemonic;
  return {};
}

std::optional<Param> param_from_mnemonic(std::string_view m) noexcept {
  for (auto& e : kMap) if (e.mnemonic == m) return e.p;
  return std::nullopt;
}

std::string_view name(Param p) noexcept {
  for (auto& e : kMap) if (e.p == p) return e.name;
  return {};
}

std::optional<Param> param_from_name(std::string_view n) noexcept {
  for (auto& e : kMap) if (e.name == n) return e.p;
  return std::nullopt;
}

Result<Command> set_source_param(Param p, double value, std::string_view send_terminator) {
  return set_source_param(mnemonic(p), value, send_terminator);
}

Result<Command> get_source_param(Param p, std::string_view send_terminator) {
  return get_source_param(mnemonic(p), send_terminator);
}

Result<Command> set_source_param(std::string_view vendor_name, double value,
                                 std::string_view send_terminator) {
  if (!vendor_name_ok(vendor_name)) return fail(ErrorKind::Config, "invalid NGX param name");
  if (!std::isfinite(value)) return fail(ErrorKind::Config, "non-finite NGX param value");
  // SR set_hv: "SSO IE, {}" -- space after the comma.
  return cmd("SSO " + std::string(vendor_name) + ", " + format_float(value), send_terminator);
}

Result<Command> get_source_param(std::string_view vendor_name, std::string_view send_terminator) {
  if (!vendor_name_ok(vendor_name)) return fail(ErrorKind::Config, "invalid NGX param name");
  return cmd("GSO " + std::string(vendor_name), send_terminator);
}

Result<Command> set_source_output(std::string_view n, double value, std::string_view send_terminator) {
  if (!vendor_name_ok(n)) return fail(ErrorKind::Config, "invalid NGX source output name");
  if (!std::isfinite(value)) return fail(ErrorKind::Config, "non-finite NGX source output value");
  return cmd("SetSourceOutput " + std::string(n) + "," + format_float(value), send_terminator);
}

Result<Command> get_source_output(std::string_view key, std::string_view send_terminator) {
  if (!vendor_name_ok(key)) return fail(ErrorKind::Config, "invalid NGX source output key");
  return cmd("GetSourceOutput " + std::string(key), send_terminator);
}

Result<Command> get_source_output(Param p, std::string_view send_terminator) {
  return get_source_output(mnemonic(p), send_terminator);
}

Result<Command> open_valve(std::string_view address, std::string_view send_terminator) {
  return named("OpenValve", address, "valve address", send_terminator);
}

Result<Command> close_valve(std::string_view address, std::string_view send_terminator) {
  return named("CloseValve", address, "valve address", send_terminator);
}

Result<Command> get_valve_status(std::string_view address, std::string_view send_terminator) {
  return named("GetValveStatus", address, "valve address", send_terminator);
}

std::string_view error_text(int code) noexcept {
  switch (code) {  // IX ERRORS
    case 1: return "ERR_INVALID_COMMAND";
    case 2: return "ERR_INVALID_PARAM";
    case 3: return "ERR_OUT_OF_RANGE";
    case 4: return "ERR_INVALID_MNEMONIC";
    case 5: return "ERR_MISSING_PARAMS";
    case 20: return "ERR_ABORTEDBY_SYSTEM";
    case 21: return "ERR_ABORTEDBY_USER";
    case 30: return "ERR_HARDWARE_MISSING";
    case 31: return "ERR_HARDWARE_FAULT";
    case 32: return "ERR_TIMEOUT";
    case 40: return "ERR_AVAILABLE";
    case 41: return "ERR_BUSY";
    case 42: return "ERR_ACCESS_DENIED";
    case 43: return "ERR_NOT_AVAILABLE";
    case 44: return "ERR_NO_RESULTS_AVAILABLE";
    case 45: return "ERR_UNIT_NOT_IN_TRIP_STATE";
    default: return "ERR_UNKNOWN";  // E99 and codes Python does not list
  }
}

ErrorKind error_kind(int code) noexcept {
  switch (code) {
    case 2: case 3: case 4: case 5: return ErrorKind::Config;
    case 20: case 21: return ErrorKind::Cancelled;
    case 30: case 31: case 41: case 43: return ErrorKind::Io;
    case 32: return ErrorKind::Timeout;
    case 42: return ErrorKind::NotConnected;
    default: return ErrorKind::Protocol;
  }
}

Result<void> decode_ok(const Bytes& reply) {
  auto t = reply_text(reply);
  if (!t) return fail(std::move(t).error());
  if (*t != "E00") return protocol_error("expected E00", reply);
  return {};
}

Result<double> decode_mass(const Bytes& reply) {
  auto t = reply_text(reply);
  if (!t) return fail(std::move(t).error());
  return number(*t, reply);
}

Result<Readback> decode_source_param(const Bytes& reply) {
  auto t = reply_text(reply);
  if (!t) return fail(std::move(t).error());
  auto f = split(*t, ',');
  if (f.size() != 2) return protocol_error("expected <setpoint>,<readback>", reply);
  auto sp = number(f[0], reply);
  if (!sp) return fail(std::move(sp).error());
  auto a = number(f[1], reply);
  if (!a) return fail(std::move(a).error());
  return Readback{*sp, *a};
}

Result<bool> decode_valve_status(const Bytes& reply) {
  auto t = reply_text(reply);
  if (!t) return fail(std::move(t).error());
  if (*t == "OPEN") return true;
  if (*t == "CLOSED") return false;
  if (*t == "E00") return protocol_error("E00 in place of valve status (pychron re-queries)", reply);
  return protocol_error("expected OPEN or CLOSED", reply);
}

Result<AcqFrame> decode_acq_event(const Bytes& line) {
  auto text = strip_line(line);
  if (!text) return fail(std::move(text).error());
  std::string_view body = *text;
  constexpr std::string_view kAcq = "#EVENT:ACQ,";
  constexpr std::string_view kAcqB = "#EVENT:ACQ.B,";
  AcqFrame fr;
  if (body.starts_with(kAcq)) fr.baseline = false;
  else if (body.starts_with(kAcqB)) fr.baseline = true;
  else return protocol_error("not an ACQ event", line);

  // SP: args = line.split(","); len >= 6; args[4] clock; args[5:] floats.
  auto parts = split(body, ',');
  if (parts.size() < 6) return protocol_error("ACQ event needs rcs id, 2 fields, time and >=1 value", line);
  fr.rcs_id = std::string(parts[1]);
  fr.field2 = std::string(parts[2]);
  fr.field3 = std::string(parts[3]);
  auto t = parse_clock(parts[4]);
  if (!t) return protocol_error("bad ACQ time \"" + std::string(parts[4]) + "\"", line);
  fr.time = *t;
  // Wire order is reversed detector order (Python zips with detectors[::-1]);
  // reverse into channel order.
  for (std::size_t i = parts.size(); i-- > 5;) {
    auto v = parse_decimal(trim(parts[i]));
    if (!v) return protocol_error("bad ACQ value \"" + std::string(parts[i]) + "\"", line);
    fr.values.push_back(*v);
  }
  return fr;
}

std::vector<Result<Message>> Demultiplexer::feed(const Bytes& chunk) {
  buffer_ += to_string(chunk);
  std::vector<Result<Message>> out;
  // Search the whole buffer so a terminator split across chunks is found.
  for (;;) {
    auto nl = buffer_.find('\n');
    if (nl == std::string::npos) break;
    std::string line = buffer_.substr(0, nl + 1);
    buffer_.erase(0, nl + 1);

    if (!std::string_view(line).starts_with(kEventPrefix)) {
      out.emplace_back(Message{Reply{to_bytes(line)}});
      continue;
    }
    auto stripped = strip_line(to_bytes(line));
    std::string body = stripped ? stripped->substr(kEventPrefix.size()) : std::string{};
    if (!body.empty() && body[0] == ':') body.erase(0, 1);
    std::string ev = body.substr(0, body.find(','));
    if (ev == "ACQ" || ev == "ACQ.B") {
      auto fr = decode_acq_event(to_bytes(line));
      if (fr) out.emplace_back(Message{std::move(*fr)});
      else out.emplace_back(fail(std::move(fr).error()));
    } else {
      out.emplace_back(Message{OtherEvent{ev, body.size() > ev.size() ? body.substr(ev.size() + 1) : std::string{}}});
    }
  }
  return out;
}

}  // namespace pychron::codec::ngx
