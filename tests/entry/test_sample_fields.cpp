#include <gtest/gtest.h>

#include "pychron/entry/sample_fields.hpp"

using namespace pychron::entry;

TEST(SampleFields, LatLon) {
  EXPECT_TRUE(check_lat_lon(std::nullopt, std::nullopt));
  EXPECT_TRUE(check_lat_lon(34.0, -106.9));
  EXPECT_TRUE(check_lat_lon(-90.0, 180.0));
  EXPECT_FALSE(check_lat_lon(34.0, std::nullopt));
  EXPECT_FALSE(check_lat_lon(std::nullopt, 1.0));
  EXPECT_FALSE(check_lat_lon(91.0, 0.0));
  EXPECT_FALSE(check_lat_lon(0.0, -181.0));
}

// Reference values from PROJ 9 via pyproj (EPSG:326zz / 327zz to EPSG:4326).
TEST(SampleFields, UtmToLatLon) {
  struct Case {
    double e, n;
    const char* zone;
    double lat, lon;
  };
  const Case cases[] = {
      {500000.0, 0.0, "31N", 0.0, 3.0},
      {323394.0, 4307396.0, "13S", 38.8977032, -107.0365035},
      {712345.0, 3876543.0, "12S", 35.0092446, -108.6729324},
      {334786.0, 6252080.0, "56H", -33.8586639, 151.2140229},  // band H: southern hemisphere
      {500000.0, 9999999.0, "19L", -0.000009, -69.0},
  };
  for (const auto& c : cases) {
    auto ll = utm_to_lat_lon(c.e, c.n, c.zone);
    ASSERT_TRUE(ll) << c.zone;
    EXPECT_NEAR(ll->lat, c.lat, 1e-6) << c.zone;
    EXPECT_NEAR(ll->lon, c.lon, 1e-6) << c.zone;
  }
  EXPECT_FALSE(utm_to_lat_lon(1, 1, "61N"));
  EXPECT_FALSE(utm_to_lat_lon(1, 1, "13"));
  EXPECT_FALSE(utm_to_lat_lon(1, 1, "13I"));
  EXPECT_FALSE(utm_to_lat_lon(1, 1, "x3N"));
}
