#include "pychron/entry/sample_fields.hpp"

#include <cctype>
#include <cmath>
#include <numbers>
#include <string>

namespace pychron::entry {

Result<void> check_lat_lon(std::optional<double> lat, std::optional<double> lon) {
  if (lat.has_value() != lon.has_value()) return fail(ErrorKind::Config, "latitude and longitude go together");
  if (lat && !(std::isfinite(*lat) && *lat >= -90 && *lat <= 90))
    return fail(ErrorKind::Config, "latitude " + std::to_string(*lat) + " is not in [-90, 90]");
  if (lon && !(std::isfinite(*lon) && *lon >= -180 && *lon <= 180))
    return fail(ErrorKind::Config, "longitude " + std::to_string(*lon) + " is not in [-180, 180]");
  return {};
}

Result<LatLon> utm_to_lat_lon(double easting, double northing, std::string_view zone_text) {
  std::string z;
  for (char c : zone_text)
    if (!std::isspace(static_cast<unsigned char>(c))) z.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  const auto bad = [&] { return fail(ErrorKind::Config, "UTM zone '" + std::string(zone_text) + "': write e.g. 13S"); };
  if (z.size() < 2 || z.size() > 3) return bad();
  const char band = z.back();
  const std::string number = z.substr(0, z.size() - 1);
  for (char c : number)
    if (c < '0' || c > '9') return bad();
  const int zone = std::stoi(number);
  if (zone < 1 || zone > 60 || band < 'C' || band > 'X' || band == 'I' || band == 'O') return bad();
  if (!std::isfinite(easting) || !std::isfinite(northing)) return fail(ErrorKind::Config, "UTM easting and northing must be numbers");
  const bool south = band < 'N';

  // Inverse transverse Mercator on WGS84 (Snyder, USGS PP 1395, eq. 8-18 ff).
  constexpr double a = 6378137.0;
  constexpr double f = 1 / 298.257223563;
  constexpr double k0 = 0.9996;
  const double e2 = f * (2 - f);
  const double ep2 = e2 / (1 - e2);
  const double x = easting - 500000.0;
  const double y = south ? northing - 10000000.0 : northing;
  const double m = y / k0;
  const double mu = m / (a * (1 - e2 / 4 - 3 * e2 * e2 / 64 - 5 * e2 * e2 * e2 / 256));
  const double e1 = (1 - std::sqrt(1 - e2)) / (1 + std::sqrt(1 - e2));
  const double phi1 = mu + (3 * e1 / 2 - 27 * std::pow(e1, 3) / 32) * std::sin(2 * mu) +
                      (21 * e1 * e1 / 16 - 55 * std::pow(e1, 4) / 32) * std::sin(4 * mu) +
                      (151 * std::pow(e1, 3) / 96) * std::sin(6 * mu) + (1097 * std::pow(e1, 4) / 512) * std::sin(8 * mu);
  const double s = std::sin(phi1), c = std::cos(phi1), t = std::tan(phi1);
  const double n1 = a / std::sqrt(1 - e2 * s * s);
  const double t1 = t * t;
  const double c1 = ep2 * c * c;
  const double r1 = a * (1 - e2) / std::pow(1 - e2 * s * s, 1.5);
  const double d = x / (n1 * k0);
  const double lat =
      phi1 - (n1 * t / r1) * (d * d / 2 - (5 + 3 * t1 + 10 * c1 - 4 * c1 * c1 - 9 * ep2) * std::pow(d, 4) / 24 +
                              (61 + 90 * t1 + 298 * c1 + 45 * t1 * t1 - 252 * ep2 - 3 * c1 * c1) * std::pow(d, 6) / 720);
  const double lon0 = (zone - 1) * 6 - 180 + 3;
  const double lon = (d - (1 + 2 * t1 + c1) * std::pow(d, 3) / 6 +
                      (5 - 2 * c1 + 28 * t1 - 3 * c1 * c1 + 8 * ep2 + 24 * t1 * t1) * std::pow(d, 5) / 120) /
                     c;
  constexpr double deg = 180 / std::numbers::pi;
  LatLon out{lat * deg, lon0 + lon * deg};
  if (auto r = check_lat_lon(out.lat, out.lon); !r) return fail(r.error());
  return out;
}

}  // namespace pychron::entry
