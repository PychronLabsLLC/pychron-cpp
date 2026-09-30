#include "pychron/codecs/isotopx_ngx.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace pychron::codec::ngx {

namespace {

const ReadSpec& line_reply() {
  static const ReadSpec spec = ReadSpec::until(kTerminator);
  return spec;
}

Command cmd(const std::string& body) {
  return Command::ascii(body + std::string(kTerminator), line_reply());
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.6g", v);
  // snprintf honours the C locale only by default; normalise just in case.
  std::string s = buf;
  std::replace(s.begin(), s.end(), ',', '.');
  return s;
}

bool clean(std::string_view s) {
  return !s.empty() && std::none_of(s.begin(), s.end(), [](char c) {
    return c == ',' || c == '#' || c == '\n' || c == '\r';
  });
}

bool vendor_name_ok(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
  });
}

struct MapEntry { Param p; std::string_view name; };
constexpr std::array<MapEntry, 19> kMap{{
    {Param::HV, "HV"}, {Param::TrapCurrent, "TC"}, {Param::TrapVoltage, "TV"},
    {Param::Emission, "EM"}, {Param::ElectronEnergy, "EE"}, {Param::IonRepeller, "IR"},
    {Param::ExtractionLens, "EL"}, {Param::ExtractionFocus, "EF"},
    {Param::ExtractionSymmetry, "ES"}, {Param::YSymmetry, "YF"}, {Param::ZSymmetry, "ZS"},
    {Param::ZFocus, "ZF"}, {Param::HorizontalSymmetry, "HS"}, {Param::Flatapole, "FP"},
    {Param::RotationQuad, "RQ"}, {Param::PoleN, "PN"}, {Param::PoleS, "PS"},
    {Param::ESAPlus, "E+"}, {Param::ESAMinus, "E-"},
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

// Body of a reply line: "OK" fields, or an Exx error.
Result<std::vector<std::string>> reply_fields(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(std::move(text).error());
  auto parts = split(*text, ',');
  std::string_view head = parts[0];
  if (head == "OK") {
    std::vector<std::string> f;
    for (std::size_t i = 1; i < parts.size(); ++i) f.emplace_back(parts[i]);
    return f;
  }
  if (head.size() == 3 && head[0] == 'E' && head[1] >= '0' && head[1] <= '9' && head[2] >= '0' && head[2] <= '9') {
    int code = (head[1] - '0') * 10 + (head[2] - '0');
    std::string what = "NGX error " + std::string(head) + " (" + std::string(error_text(code)) + ")";
    if (parts.size() > 1) what += ": " + std::string(text->substr(4));
    return fail(error_kind(code), what + ": reply \"" + escape(reply) + "\"");
  }
  return protocol_error("expected OK or Exx", reply);
}

Result<double> number(const std::string& s, const Bytes& reply) {
  auto v = parse_decimal(s);
  if (!v) return protocol_error("unparsable number \"" + s + "\"", reply);
  return *v;
}

}  // namespace

Result<Command> login(std::string_view user, std::string_view password) {
  if (!clean(user) || !clean(password)) return fail(ErrorKind::Config, "invalid NGX login credentials");
  return cmd("Login " + std::string(user) + "," + std::string(password));
}

Result<Command> set_mass(double mass) {
  if (!std::isfinite(mass) || mass < 0) return fail(ErrorKind::Config, "invalid NGX mass");
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.4f", mass);
  return cmd(std::string("SetMass ") + buf);
}

Command get_mass() { return cmd("GetMass"); }

Result<Command> start_acq(int count) {
  if (count < 1) return fail(ErrorKind::Config, "NGX StartAcq count must be >= 1");
  return cmd("StartAcq " + std::to_string(count));
}

Command stop_acq() { return cmd("StopAcq"); }

Result<Command> set_acq_period(int seconds) {
  if (seconds < 1) return fail(ErrorKind::Config, "NGX acquisition period must be >= 1 s");
  return cmd("SetAcqPeriod " + std::to_string(seconds));
}

Command sab() { return cmd("SAB"); }

std::optional<std::string_view> mnemonic(Param p) noexcept {
  for (auto& e : kMap) if (e.p == p) return e.name;
  return std::nullopt;
}

std::optional<Param> param_from_mnemonic(std::string_view name) noexcept {
  for (auto& e : kMap) if (e.name == name) return e.p;
  return std::nullopt;
}

Result<Command> set_source_param(Param p, double value) {
  auto m = mnemonic(p);
  if (!m) return fail(ErrorKind::Config, "param not supported by NGX");
  return set_source_param(*m, value);
}

Result<Command> get_source_param(Param p) {
  auto m = mnemonic(p);
  if (!m) return fail(ErrorKind::Config, "param not supported by NGX");
  return get_source_param(*m);
}

Result<Command> set_source_param(std::string_view name, double value) {
  if (!vendor_name_ok(name) && !param_from_mnemonic(name)) return fail(ErrorKind::Config, "invalid NGX param name");
  if (!std::isfinite(value)) return fail(ErrorKind::Config, "non-finite NGX param value");
  return cmd("SSO " + std::string(name) + "," + num(value));
}

Result<Command> get_source_param(std::string_view name) {
  if (!vendor_name_ok(name) && !param_from_mnemonic(name)) return fail(ErrorKind::Config, "invalid NGX param name");
  return cmd("GSO " + std::string(name));
}

std::string_view error_text(int code) noexcept {
  switch (code) {
    case 1: return "unknown command";
    case 2: return "bad argument";
    case 3: return "busy";
    case 4: return "not logged in";
    case 5: return "value out of range";
    case 6: return "acquisition not running";
    case 7: return "interlock";
    case 8: return "timeout";
    default: return "unknown error";
  }
}

ErrorKind error_kind(int code) noexcept {
  switch (code) {
    case 2: case 5: return ErrorKind::Config;
    case 4: return ErrorKind::NotConnected;
    case 7: return ErrorKind::Interlock;
    case 8: return ErrorKind::Timeout;
    default: return ErrorKind::Protocol;
  }
}

Result<void> decode_ok(const Bytes& reply) {
  auto f = reply_fields(reply);
  if (!f) return fail(std::move(f).error());
  return {};
}

Result<double> decode_mass(const Bytes& reply) {
  auto f = reply_fields(reply);
  if (!f) return fail(std::move(f).error());
  if (f->size() != 1) return protocol_error("expected OK,<mass>", reply);
  return number((*f)[0], reply);
}

Result<Readback> decode_source_param(const Bytes& reply) {
  auto f = reply_fields(reply);
  if (!f) return fail(std::move(f).error());
  if (f->empty() || f->size() > 2) return protocol_error("expected OK,<setpoint>[,<readback>]", reply);
  Readback rb;
  auto sp = number((*f)[0], reply);
  if (!sp) return fail(std::move(sp).error());
  rb.setpoint = *sp;
  if (f->size() == 2) {
    auto a = number((*f)[1], reply);
    if (!a) return fail(std::move(a).error());
    rb.actual = *a;
  }
  return rb;
}

Result<AcqFrame> decode_acq_event(const Bytes& line) {
  auto text = strip_terminator(line, kTerminator);
  if (!text) return fail(std::move(text).error());
  std::string_view body = *text;
  if (!body.starts_with(kEventPrefix)) return protocol_error("not an event line", line);
  body.remove_prefix(kEventPrefix.size());
  auto parts = split(body, ',');
  bool baseline;
  if (parts[0] == "ACQ") baseline = false;
  else if (parts[0] == "ACQ.B") baseline = true;
  else return protocol_error("not an ACQ event", line);
  if (parts.size() < 4) return protocol_error("ACQ event needs seq, time and >=1 value", line);

  AcqFrame fr;
  fr.baseline = baseline;
  auto seq = parse_decimal(parts[1]);
  auto ts = parse_decimal(parts[2]);
  if (!seq || *seq < 0 || *seq != std::floor(*seq)) return protocol_error("bad ACQ sequence", line);
  if (!ts) return protocol_error("bad ACQ timestamp", line);
  fr.seq = static_cast<std::uint64_t>(*seq);
  fr.ts_s = *ts;
  for (std::size_t i = parts.size(); i-- > 3;) {  // reverse detector order -> channel order
    auto v = parse_decimal(parts[i]);
    if (!v) return protocol_error("bad ACQ value \"" + std::string(parts[i]) + "\"", line);
    fr.values.push_back(*v);
  }
  return fr;
}

std::vector<Result<Message>> Demultiplexer::feed(const Bytes& chunk) {
  buffer_ += to_string(chunk);
  std::vector<Result<Message>> out;
  for (;;) {
    auto end = buffer_.find(kTerminator);
    if (end == std::string::npos) break;
    std::size_t len = end + kTerminator.size();
    Bytes raw = to_bytes(std::string_view(buffer_).substr(0, len));
    buffer_.erase(0, len);

    if (!std::string_view(to_string(raw)).starts_with(kEventPrefix)) {
      out.emplace_back(Message{Reply{std::move(raw)}});
      continue;
    }
    std::string body = to_string(raw).substr(kEventPrefix.size(), len - kEventPrefix.size() - kTerminator.size());
    std::string name = body.substr(0, body.find(','));
    if (name == "ACQ" || name == "ACQ.B") {
      auto fr = decode_acq_event(raw);
      if (fr) out.emplace_back(Message{std::move(*fr)});
      else out.emplace_back(fail(std::move(fr).error()));
    } else {
      out.emplace_back(Message{OtherEvent{name, body.size() > name.size() ? body.substr(name.size() + 1) : std::string{}}});
    }
  }
  return out;
}

}  // namespace pychron::codec::ngx
