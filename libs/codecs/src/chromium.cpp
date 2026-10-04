#include "pychron/codecs/chromium.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>

namespace pychron::codec::chromium {

namespace {

codec::Command query(std::string_view text) {
  return codec::Command::ascii(std::string(text) + std::string(kCommandTerminator), reply_frame());
}

codec::Command action(std::string_view text) {
  return codec::Command::write_only(to_bytes(std::string(text) + std::string(kCommandTerminator)));
}

std::string_view trim(std::string_view s) {
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && blank(s.front())) s.remove_prefix(1);
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}

// Locale-free; the whole of `s` must be the number. Not std::from_chars:
// its floating-point overloads are missing from older libc++.
std::optional<double> number(std::string_view s) { return codec::parse_decimal(trim(s)); }

std::vector<std::string_view> split(std::string_view s, char by) {
  std::vector<std::string_view> out;
  while (true) {
    const auto at = s.find(by);
    out.push_back(trim(s.substr(0, at)));
    if (at == std::string_view::npos) break;
    s.remove_prefix(at + 1);
  }
  return out;
}

// The reply's text, or the error a "?<n>" reply stands for.
Result<std::string> text_of(const Bytes& reply) {
  if (const auto code = error_code(reply)) return fail(to_error(*code));
  return std::string(trim(to_string(reply)));
}

Result<std::array<double, 3>> triple(const Bytes& reply, std::string_view what) {
  auto text = text_of(reply);
  if (!text) return fail(std::move(text).error());
  const auto parts = split(*text, ',');
  std::array<double, 3> out{};
  if (parts.size() != 3) return protocol_error(std::string(what) + " is not three values", reply);
  for (std::size_t i = 0; i < 3; ++i) {
    const auto v = number(parts[i]);
    if (!v) return protocol_error(std::string(what) + " is not three numbers", reply);
    out[i] = *v;
  }
  return out;
}

std::string integer(std::int64_t v) { return std::to_string(v); }

}  // namespace

ReadSpec reply_frame() { return ReadSpec::until_any("\r\n"); }

codec::Command sys_id() { return query("Sys.ID?"); }
codec::Command laser_status() { return query("Laser.Status?"); }
codec::Command laser_interlocks() { return query("Laser.Interlocks?"); }
codec::Command laser_enabled() { return query("Laser.Enable?"); }
codec::Command laser_output_query() { return query("Laser.Output?"); }
codec::Command stage_position() { return query("Stage.Pos?"); }
codec::Command stage_limits() { return query("Stage.Status?"); }
codec::Command scans_count() { return query("Scans.Count?"); }

Result<codec::Command> scan_in_position(int scan) {
  if (scan < 1) return fail(ErrorKind::Config, "scan " + std::to_string(scan) + ": scans are numbered from 1");
  return query("Scans.InPos? " + std::to_string(scan));
}

codec::Command laser_enable(bool on) { return action(on ? "Laser.Enable 1" : "Laser.Enable 0"); }

Result<codec::Command> laser_output(double percent) {
  if (!std::isfinite(percent) || percent < 0 || percent > 100) {
    return fail(ErrorKind::Config, "laser output must be 0 to 100 percent");
  }
  // Thousandths, then the shortest decimal for that: no locale, no exponent.
  const double rounded = std::round(percent * 1000.0) / 1000.0;
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), rounded, std::chars_format::fixed, 3);
  std::string text(buffer.data(), ec == std::errc{} ? end : buffer.data());
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  return action("Laser.Output " + text);
}

codec::Command laser_fire() { return action("Laser.Fire"); }
codec::Command laser_stop() { return action("Laser.Stop"); }

codec::Command stage_move_to(Microns target, Microns speed) {
  return action("Stage.MoveTo " + integer(target.x) + "," + integer(target.y) + "," + integer(target.z) + "," +
                integer(speed.x) + "," + integer(speed.y) + "," + integer(speed.z));
}

codec::Command stage_stop() { return action("Stage.Stop"); }

Result<codec::Command> scan_move_to(int scan) {
  if (scan < 1) return fail(ErrorKind::Config, "scan " + std::to_string(scan) + ": scans are numbered from 1");
  return action("Scans.MoveTo " + std::to_string(scan));
}

codec::Command scans_stop() { return action("Scans.Stop"); }
codec::Command scans_status_verbosity(int value) { return action("Scans.Status_Verbosity " + std::to_string(value)); }

std::optional<int> error_code(const Bytes& reply) {
  const std::string raw = to_string(reply);
  const std::string_view text = trim(raw);
  if (text.size() != 2 || text[0] != '?' || text[1] < '0' || text[1] > '4') return std::nullopt;
  return text[1] - '0';
}

Error to_error(int code, std::string_view command) {
  ErrorKind kind = ErrorKind::Protocol;
  std::string why;
  switch (code) {
    case 0: why = "command not implemented"; break;
    case 1: why = "unknown component"; break;
    case 2: why = "unknown command"; break;
    case 3:
      kind = ErrorKind::Config;
      why = "a value is wrong or missing";
      break;
    case 4:
      kind = ErrorKind::Io;
      why = "not supported by this hardware or failed";
      break;
    default: why = "unknown error"; break;
  }
  Error e;
  e.kind = kind;
  e.what = "Chromium refused " + (command.empty() ? std::string("the command") : std::string(command)) + " (?" +
           std::to_string(code) + "): " + why;
  e.code = "chromium?" + std::to_string(code);
  return e;
}

Result<std::string> decode_text(const Bytes& reply) { return text_of(reply); }

Result<bool> decode_flag(const Bytes& reply) {
  auto text = text_of(reply);
  if (!text) return fail(std::move(text).error());
  if (*text == "1") return true;
  if (*text == "0") return false;
  return protocol_error("expected 0 or 1", reply);
}

Result<double> decode_number(const Bytes& reply) {
  auto text = text_of(reply);
  if (!text) return fail(std::move(text).error());
  const auto v = number(*text);
  if (!v) return protocol_error("expected a number", reply);
  return *v;
}

Result<Microns> decode_position(const Bytes& reply) {
  auto v = triple(reply, "stage position");
  if (!v) return fail(std::move(v).error());
  return Microns{std::llround((*v)[0]), std::llround((*v)[1]), std::llround((*v)[2])};
}

Result<std::vector<std::string>> decode_interlocks(const Bytes& reply) {
  auto text = text_of(reply);
  if (!text) return fail(std::move(text).error());
  std::vector<std::string> out;
  for (const auto part : split(*text, ','))
    if (!part.empty()) out.emplace_back(part);
  return out;
}

Result<LimitStatus> decode_limits(const Bytes& reply) {
  auto v = triple(reply, "limit status");
  if (!v) return fail(std::move(v).error());
  std::array<int, 3> out{};
  for (std::size_t i = 0; i < 3; ++i) {
    if ((*v)[i] != -1 && (*v)[i] != 0 && (*v)[i] != 1) return protocol_error("limit status is not -1, 0 or 1", reply);
    out[i] = static_cast<int>((*v)[i]);
  }
  return LimitStatus{out[0], out[1], out[2]};
}

Result<std::string> decode_id(const Bytes& reply) {
  auto text = text_of(reply);
  if (!text) return fail(std::move(text).error());
  constexpr std::string_view kName = "CHROMIUM";
  bool ours = text->size() >= kName.size();
  for (std::size_t i = 0; ours && i < kName.size(); ++i) {
    ours = std::toupper(static_cast<unsigned char>((*text)[i])) == kName[i];
  }
  if (!ours) return protocol_error("not a Chromium: it answered \"" + *text + "\"", reply);
  return *text;
}

}  // namespace pychron::codec::chromium
