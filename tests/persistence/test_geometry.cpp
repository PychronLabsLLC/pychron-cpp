// The sample location column (sql/geometry.hpp): EWKT written on both
// engines, EWKT or hex EWKB read back.

#include "sql/geometry.hpp"

#include <gtest/gtest.h>

using namespace pychron::persistence;
using namespace pychron::persistence::detail;

TEST(Geometry, WritesEwktLongitudeFirst) {
  EXPECT_EQ(ewkt_point(34.0722, -106.905), "SRID=4326;POINT(-106.905 34.0722)");
  EXPECT_EQ(ewkt_point(0, 0), "SRID=4326;POINT(0 0)");
  // 15 significant digits, as ST_AsEWKT prints: a coordinate with that many
  // (a millimetre has nine) reads back as the same double, so an edit's
  // expected value matches what the store holds.
  const auto p = parse_point(ewkt_point(37.1234567890123, -106.987654321098));
  ASSERT_TRUE(p);
  EXPECT_EQ(p->lat, 37.1234567890123);
  EXPECT_EQ(p->lon, -106.987654321098);
  EXPECT_EQ(ewkt_point(37.1234567890123, -106.987654321098), "SRID=4326;POINT(-106.987654321098 37.1234567890123)");
}

TEST(Geometry, ReadsEwktWithOrWithoutSrid) {
  const auto a = parse_point("SRID=4326;POINT(-106.905 34.0722)");
  ASSERT_TRUE(a);
  EXPECT_EQ(*a, (GeoPoint{34.0722, -106.905}));
  const auto b = parse_point(" POINT(1.5 -2.25) ");
  ASSERT_TRUE(b);
  EXPECT_EQ(*b, (GeoPoint{-2.25, 1.5}));
  EXPECT_FALSE(parse_point(""));
  EXPECT_FALSE(parse_point("POINT EMPTY"));
  EXPECT_FALSE(parse_point("LINESTRING(0 0, 1 1)"));
  EXPECT_FALSE(parse_point("SRID=4326;POINT(1)"));
  EXPECT_FALSE(parse_point("POINT(x y)"));
  EXPECT_FALSE(parse_point("POINT(nan 1)"));
}

TEST(Geometry, ReadsHexEwkbAsPostgresqlReturnsIt) {
  // Little-endian with an SRID (what a bare SELECT of a PostGIS column gives).
  const auto le = parse_point("0101000020E610000052b81e85ebb95ac0b7627fd93d094140");
  ASSERT_TRUE(le);
  EXPECT_DOUBLE_EQ(le->lat, 34.0722);
  EXPECT_DOUBLE_EQ(le->lon, -106.905);
  // Big-endian, no SRID.
  const auto be = parse_point("0000000001c05ab9eb851eb8524041093dd97f62b7");
  ASSERT_TRUE(be);
  EXPECT_DOUBLE_EQ(be->lat, 34.0722);
  EXPECT_DOUBLE_EQ(be->lon, -106.905);
  EXPECT_FALSE(parse_point("0101000020E610000052b81e85ebb95ac0b7627fd93d0941"));           // short
  EXPECT_FALSE(parse_point("0102000020E610000052b81e85ebb95ac0b7627fd93d094140"));  // a linestring
  EXPECT_FALSE(parse_point("zz01000020E610000052b81e85ebb95ac0b7627fd93d094140"));                // not hex
}

TEST(Geometry, ReadExpressionPerDialect) {
  EXPECT_EQ(geom_read(Dialect::PostgreSql, QStringLiteral("s.geom")), QStringLiteral("public.ST_AsEWKT(s.geom)"));
  EXPECT_EQ(geom_read(Dialect::Sqlite, QStringLiteral("s.geom")), QStringLiteral("s.geom"));
}
