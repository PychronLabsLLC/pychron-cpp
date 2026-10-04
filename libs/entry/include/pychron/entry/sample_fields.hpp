#pragma once

// Checks on a sample's location fields (sample and package entry spec,
// section 6, sample_fields.hpp).

#include <optional>
#include <string_view>

#include "pychron/core/error.hpp"

namespace pychron::entry {

// Latitude in [-90, 90] and longitude in [-180, 180], both or neither.
Result<void> check_lat_lon(std::optional<double> lat, std::optional<double> lon);

struct LatLon {
  double lat = 0, lon = 0;
};

// WGS84 latitude and longitude of a UTM coordinate. `zone` is the zone
// number and latitude band, "13S" or "5N"; a band letter before 'N' is the
// southern hemisphere (legacy importer.py:393-451).
Result<LatLon> utm_to_lat_lon(double easting, double northing, std::string_view zone);

}  // namespace pychron::entry
