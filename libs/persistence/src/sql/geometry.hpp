#pragma once

// The sample location column (migration 0004): a PostGIS geometry(Point,
// 4326) on PostgreSQL, the same point as EWKT text on SQLite. Both engines
// are written EWKT ('SRID=4326;POINT(lon lat)'; PostGIS parses it) and read
// through geom_read(): ST_AsEWKT on PostgreSQL, the text itself on SQLite.
// parse_point() also takes hex EWKB, which is what a bare SELECT of the
// column returns on PostgreSQL, so white-box tests can read either.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <QString>

#include "pychron/persistence/store.hpp"

namespace pychron::persistence::detail {

inline constexpr int kWgs84 = 4326;

struct GeoPoint {
  double lat = 0, lon = 0;
  friend bool operator==(const GeoPoint&, const GeoPoint&) = default;
};

// 'SRID=4326;POINT(lon lat)', coordinates to 15 significant digits (what
// ST_AsEWKT prints), so a value written and read back compares equal.
inline std::string ewkt_point(double lat, double lon) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "SRID=%d;POINT(%.15g %.15g)", kWgs84, lon, lat);
  return buf;
}

// The column as the store reads it: `expr` is the column reference ("s.geom").
inline QString geom_read(Dialect dialect, const QString& expr) {
  if (dialect == Dialect::PostgreSql) return QStringLiteral("public.ST_AsEWKT(%1)").arg(expr);
  return expr;
}

namespace geometry_detail {

inline std::optional<double> number(std::string_view text) {
  double v = 0;
  const auto* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, v);
  if (r.ec != std::errc{} || r.ptr != end || !std::isfinite(v)) return std::nullopt;
  return v;
}

inline std::optional<std::uint64_t> hex(std::string_view text) {
  std::uint64_t v = 0;
  const auto r = std::from_chars(text.data(), text.data() + text.size(), v, 16);
  if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) return std::nullopt;
  return v;
}

// 'POINT(x y)' or 'SRID=n;POINT(x y)'.
inline std::optional<GeoPoint> ewkt(std::string_view text) {
  if (text.rfind("SRID=", 0) == 0) {
    const auto semi = text.find(';');
    if (semi == std::string_view::npos) return std::nullopt;
    text.remove_prefix(semi + 1);
  }
  if (text.rfind("POINT", 0) != 0) return std::nullopt;
  text.remove_prefix(5);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  if (text.empty() || text.front() != '(' || text.back() != ')') return std::nullopt;
  text = text.substr(1, text.size() - 2);
  const auto space = text.find(' ');
  if (space == std::string_view::npos) return std::nullopt;
  std::string_view y = text.substr(space + 1);
  while (!y.empty() && y.front() == ' ') y.remove_prefix(1);
  const auto x = number(text.substr(0, space));
  const auto yv = number(y);
  if (!x || !yv) return std::nullopt;
  return GeoPoint{*yv, *x};
}

// Hex EWKB of a 2D point, little- or big-endian, with or without an SRID.
inline std::optional<GeoPoint> ewkb(std::string_view text) {
  if (text.size() != 42 && text.size() != 50) return std::nullopt;
  const auto order = hex(text.substr(0, 2));
  if (!order || (*order != 0 && *order != 1)) return std::nullopt;
  const bool little = *order == 1;
  auto word = [&](std::size_t at) -> std::optional<std::uint64_t> {
    const auto raw = hex(text.substr(at, 8));
    if (!raw) return std::nullopt;
    std::uint32_t v = static_cast<std::uint32_t>(*raw);
    if (little) v = ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
    return v;
  };
  const auto type = word(2);
  if (!type) return std::nullopt;
  const bool has_srid = (*type & 0x20000000u) != 0;
  if ((*type & 0xffu) != 1 || text.size() != (has_srid ? 50u : 42u)) return std::nullopt;
  std::size_t at = has_srid ? 18 : 10;
  auto coordinate = [&]() -> std::optional<double> {
    const auto raw = hex(text.substr(at, 16));
    at += 16;
    if (!raw) return std::nullopt;
    std::uint64_t bits = *raw;
    if (little) {
      std::uint64_t swapped = 0;
      for (int i = 0; i < 8; ++i) swapped |= ((bits >> (8 * i)) & 0xffu) << (8 * (7 - i));
      bits = swapped;
    }
    double v = 0;
    std::memcpy(&v, &bits, sizeof v);
    if (!std::isfinite(v)) return std::nullopt;
    return v;
  };
  const auto x = coordinate();
  const auto y = coordinate();
  if (!x || !y) return std::nullopt;
  return GeoPoint{*y, *x};
}

}  // namespace geometry_detail

// The point of a geom column's text (EWKT or hex EWKB); nullopt when it is
// not a point.
inline std::optional<GeoPoint> parse_point(std::string_view text) {
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  if (auto p = geometry_detail::ewkt(text)) return p;
  return geometry_detail::ewkb(text);
}

}  // namespace pychron::persistence::detail
