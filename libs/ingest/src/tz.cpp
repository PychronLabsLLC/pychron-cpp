#include "pychron/ingest/tz.hpp"

#include <chrono>
#include <cstdint>
#include <string>

// The one place the time zone back end is chosen. Standard C++20 where the
// library has the tzdb; Howard Hinnant's date (same API) elsewhere.
#if PYCHRON_HAVE_STD_TZDB
namespace tzlib = std::chrono;
#else
#include <date/tz.h>
namespace tzlib = date;
#endif

namespace pychron::ingest {

namespace {

namespace ch = std::chrono;

// Reads exactly `count` digits at `pos`.
bool digits(std::string_view s, std::size_t& pos, std::size_t count, int& out) {
  if (s.size() - pos < count || pos > s.size()) return false;
  int v = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  pos += count;
  out = v;
  return true;
}

bool expect(std::string_view s, std::size_t& pos, char c) {
  if (pos >= s.size() || s[pos] != c) return false;
  ++pos;
  return true;
}

const tzlib::time_zone* find_zone(std::string_view iana) {
  try {
    return tzlib::locate_zone(std::string(iana));
  } catch (...) {
    return nullptr;
  }
}

}  // namespace

bool known_zone(std::string_view iana) { return find_zone(iana) != nullptr; }

Result<LocalToUtc> local_to_utc(std::string_view s, std::string_view iana) {
  const auto bad = [&] {
    return fail(ErrorKind::Protocol, "not a local timestamp: '" + std::string(s) + "'");
  };

  std::size_t pos = 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (!digits(s, pos, 4, y) || !expect(s, pos, '-') || !digits(s, pos, 2, mo) || !expect(s, pos, '-') ||
      !digits(s, pos, 2, d))
    return bad();
  if (pos >= s.size() || (s[pos] != ' ' && s[pos] != 'T')) return bad();
  ++pos;
  if (!digits(s, pos, 2, h) || !expect(s, pos, ':') || !digits(s, pos, 2, mi) || !expect(s, pos, ':') ||
      !digits(s, pos, 2, sec))
    return bad();

  std::int64_t frac_us = 0;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    std::size_t n = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9' && n < 6) {
      frac_us = frac_us * 10 + (s[pos] - '0');
      ++pos;
      ++n;
    }
    if (n == 0) return bad();
    for (; n < 6; ++n) frac_us *= 10;
  }
  if (pos != s.size()) return bad();

  const tzlib::year_month_day ymd{tzlib::year{y}, tzlib::month{static_cast<unsigned>(mo)},
                                  tzlib::day{static_cast<unsigned>(d)}};
  if (!ymd.ok() || h > 23 || mi > 59 || sec > 59) return bad();

  const tzlib::time_zone* zone = find_zone(iana);
  if (zone == nullptr) return fail(ErrorKind::Config, "unknown time zone '" + std::string(iana) + "'");

  const tzlib::local_time<ch::microseconds> local =
      tzlib::local_days{ymd} + ch::hours{h} + ch::minutes{mi} + ch::seconds{sec} + ch::microseconds{frac_us};

  tzlib::local_info info;
  try {
    info = zone->get_info(local);
  } catch (...) {
    return fail(ErrorKind::Config, "time zone '" + std::string(iana) + "' has no data for " + std::string(s));
  }

  LocalToUtc out;
  switch (info.result) {
    case tzlib::local_info::nonexistent:
      out.kind = LocalKind::Nonexistent;
      out.utc.micros = ch::duration_cast<ch::microseconds>(info.first.end.time_since_epoch()).count();
      break;
    case tzlib::local_info::ambiguous:
      out.kind = LocalKind::Ambiguous;
      out.utc.micros = ch::duration_cast<ch::microseconds>((local - info.first.offset).time_since_epoch()).count();
      break;
    default:
      out.utc.micros = ch::duration_cast<ch::microseconds>((local - info.first.offset).time_since_epoch()).count();
      break;
  }
  return out;
}

}  // namespace pychron::ingest
