#include "pychron/persistence/ids.hpp"

#include <chrono>
#include <cstdio>

namespace pychron::persistence {
namespace {

constexpr std::uint64_t kRandLoMask = (std::uint64_t{1} << 62) - 1;

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::int64_t system_millis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool parse_int(std::string_view s, int& out) {
  out = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    out = out * 10 + (c - '0');
  }
  return !s.empty();
}

}  // namespace

Uuid Uuid::v7() {
  static UuidV7Generator generator;
  return generator.next();
}

std::optional<Uuid> Uuid::parse(std::string_view text) {
  if (text.size() != 36) return std::nullopt;
  Bytes b{};
  std::size_t out = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return std::nullopt;
      ++i;
      continue;
    }
    const int hi = hex_value(text[i]);
    const int lo = hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    b[out++] = static_cast<std::uint8_t>(hi << 4 | lo);
    i += 2;
  }
  return Uuid(b);
}

std::string Uuid::str() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (std::size_t i = 0; i < bytes_.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
    s.push_back(kHex[bytes_[i] >> 4]);
    s.push_back(kHex[bytes_[i] & 0xf]);
  }
  return s;
}

bool Uuid::is_nil() const noexcept {
  for (auto b : bytes_)
    if (b != 0) return false;
  return true;
}

UuidV7Generator::UuidV7Generator() : UuidV7Generator(system_millis, std::random_device{}()) {}

UuidV7Generator::UuidV7Generator(MillisClock clock, std::uint64_t seed) : clock_(std::move(clock)), rng_(seed) {}

Uuid UuidV7Generator::next() {
  std::lock_guard lock(mutex_);
  const std::int64_t ms = clock_();
  if (ms > last_ms_) {
    last_ms_ = ms;
    // Top bit of rand_a stays clear so a burst within one millisecond has
    // 2^73 increments of headroom before it must borrow the next millisecond.
    rand_hi_ = rng_() & 0x7ff;
    rand_lo_ = rng_() & kRandLoMask;
  } else if (++rand_lo_ > kRandLoMask) {
    rand_lo_ = 0;
    if (++rand_hi_ > 0xfff) {
      rand_hi_ = 0;
      ++last_ms_;
    }
  }
  Uuid::Bytes b{};
  const auto ts = static_cast<std::uint64_t>(last_ms_);
  for (int i = 0; i < 6; ++i) b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(ts >> (40 - 8 * i));
  b[6] = static_cast<std::uint8_t>(0x70 | (rand_hi_ >> 8));
  b[7] = static_cast<std::uint8_t>(rand_hi_ & 0xff);
  b[8] = static_cast<std::uint8_t>(0x80 | ((rand_lo_ >> 56) & 0x3f));
  for (int i = 0; i < 7; ++i) b[static_cast<std::size_t>(9 + i)] = static_cast<std::uint8_t>(rand_lo_ >> (48 - 8 * i));
  return Uuid(b);
}

UtcTime UtcTime::now() {
  using namespace std::chrono;
  return UtcTime{duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()};
}

std::optional<UtcTime> UtcTime::parse(std::string_view s) {
  using namespace std::chrono;
  // YYYY-MM-DDTHH:MM:SS
  if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s.back() != 'Z')
    return std::nullopt;
  int y, mo, d, h, mi, se;
  if (!parse_int(s.substr(0, 4), y) || !parse_int(s.substr(5, 2), mo) || !parse_int(s.substr(8, 2), d) ||
      !parse_int(s.substr(11, 2), h) || !parse_int(s.substr(14, 2), mi) || !parse_int(s.substr(17, 2), se))
    return std::nullopt;
  std::int64_t frac = 0;
  const auto rest = s.substr(19, s.size() - 20);
  if (!rest.empty()) {
    if (rest[0] != '.' || rest.size() < 2 || rest.size() > 7) return std::nullopt;
    int digits = 0;
    if (!parse_int(rest.substr(1), digits)) return std::nullopt;
    frac = digits;
    for (std::size_t i = rest.size() - 1; i < 6; ++i) frac *= 10;
  }
  const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
  if (!ymd.ok() || h > 23 || mi > 59 || se > 60) return std::nullopt;
  const auto days = sys_days{ymd}.time_since_epoch().count();
  const std::int64_t secs = std::int64_t{days} * 86400 + h * 3600 + mi * 60 + se;
  return UtcTime{secs * 1'000'000 + frac};
}

std::string UtcTime::iso() const {
  using namespace std::chrono;
  std::int64_t secs = micros / 1'000'000;
  std::int64_t frac = micros % 1'000'000;
  if (frac < 0) {
    frac += 1'000'000;
    --secs;
  }
  std::int64_t days = secs / 86400;
  std::int64_t tod = secs % 86400;
  if (tod < 0) {
    tod += 86400;
    --days;
  }
  const year_month_day ymd{sys_days{std::chrono::days{days}}};
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%06dZ", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), static_cast<int>(tod / 3600),
                static_cast<int>(tod / 60 % 60), static_cast<int>(tod % 60), static_cast<int>(frac));
  return buf;
}

}  // namespace pychron::persistence
