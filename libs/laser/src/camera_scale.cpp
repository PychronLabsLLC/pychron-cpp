#include "pychron/laser/camera_scale.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
#include <system_error>
#include <vector>

#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/laser_system.hpp"

namespace pychron::laser {

namespace {

namespace fs = std::filesystem;

// What a solved map says about itself; Config error when it is not a camera's.
Result<ScaleMeasurement> describe(const vision::CameraStageMap& map) {
  if (!map.valid()) return fail(ErrorKind::Config, "the sightings do not make a map: the target did not move in two directions");
  ScaleMeasurement m;
  m.map = map;
  // The columns: the stage move that a pixel in x, and a pixel in y, calls for.
  const double ax = map.m[0][0], ay = map.m[1][0], bx = map.m[0][1], by = map.m[1][1];
  const double sx = std::hypot(ax, ay), sy = std::hypot(bx, by);
  if (!(sx > 0) || !(sy > 0)) return fail(ErrorKind::Config, "the sightings do not make a map: the target did not move");
  m.px_per_mm = 1.0 / std::sqrt(std::abs(ax * by - bx * ay));
  m.anisotropy = std::abs(sx - sy) / ((sx + sy) / 2);
  const double cosine = std::clamp((ax * bx + ay * by) / (sx * sy), -1.0, 1.0);
  m.skew_deg = std::abs(90.0 - std::acos(cosine) * 180.0 / std::numbers::pi);
  m.flip_x = map.m[0][0] < 0;
  m.flip_y = map.m[1][1] < 0;
  return m;
}

std::string num(double value, int places = 3) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out.setf(std::ios::fixed);
  out.precision(places);
  out << value;
  return out.str();
}

}  // namespace

Result<ScaleMeasurement> scale_from(std::span<const vision::JogPair> jogs) {
  auto map = vision::CameraStageMap::solve(jogs);
  if (!map) return fail(ErrorKind::Config, "the camera's scale cannot be told from these sightings: " + map.error().what);
  auto m = describe(*map);
  if (!m) return m;
  if (m->anisotropy > kMaxAnisotropy) {
    return fail(ErrorKind::Config, "the two axes disagree about the scale by " + num(m->anisotropy * 100, 1) +
                                       "% (at most " + num(kMaxAnisotropy * 100, 0) +
                                       "%): one sighting was not of the same target");
  }
  if (m->skew_deg > kMaxSkewDeg) {
    return fail(ErrorKind::Config, "the two axes are " + num(m->skew_deg, 1) + " degrees from square (at most " +
                                       num(kMaxSkewDeg, 0) + "): one sighting was not of the same target");
  }
  return m;
}

Result<ScaleMeasurement> measure_camera_scale(LaserSystem& system, double step_mm,
                                              const std::function<Result<void>()>& arrive) {
  if (!system.has_camera()) return fail(ErrorKind::Config, "no camera to measure", system.device_name());
  if (!(step_mm >= 0.01) || !(step_mm <= 2.0)) {
    return fail(ErrorKind::Config, "the step must be from 0.01 to 2 mm", system.device_name());
  }
  auto start = system.position();
  if (!start) return fail(std::move(start).error());

  // The target nearest `near`, in a picture taken now.
  const auto sight = [&system](const vision::Point2& near, const char* where) -> Result<vision::Point2> {
    auto seen = system.view(true, true);
    if (!seen) return fail(std::move(seen).error());
    const vision::Target* best = nullptr;
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto& target : seen->targets) {
      const double d = std::hypot(target.center_px.x - near.x, target.center_px.y - near.y);
      if (d < nearest) {
        nearest = d;
        best = &target;
      }
    }
    if (best == nullptr) {
      return fail(ErrorKind::Config, std::string("nothing to follow in the picture ") + where +
                                         ": put a hole, or any mark the finder sees, under the aim point first");
    }
    return best->center_px;
  };
  const auto go = [&](double x, double y) -> Result<void> {
    if (auto moved = system.set_xy(x, y); !moved) return moved;
    return arrive();
  };
  const auto back = [&] { return go(start->x, start->y); };

  auto first = system.view(true, true);
  if (!first) return fail(std::move(first).error());
  const vision::Point2 aim{first->aim_px.x, first->aim_px.y};
  auto p0 = sight(aim, "at the start");
  if (!p0) return fail(std::move(p0).error());

  std::vector<vision::JogPair> jogs;
  for (const auto& [dx, dy, where] : {std::tuple{step_mm, 0.0, "after the step in x"}, std::tuple{0.0, step_mm, "after the step in y"}}) {
    if (auto moved = go(start->x + dx, start->y + dy); !moved) return fail(std::move(moved).error());
    // The same target: it has moved less far than any other is away.
    auto p = sight(*p0, where);
    if (!p) {
      (void)back();
      return fail(std::move(p).error());
    }
    jogs.push_back({{dx, dy}, {p->x - p0->x, p->y - p0->y}});
  }
  if (auto returned = back(); !returned) return fail(std::move(returned).error());
  return scale_from(jogs);
}

CameraScaleStore::CameraScaleStore(fs::path dir) : dir_(std::move(dir)) {}

fs::path CameraScaleStore::file(std::string_view device) const { return dir_ / (std::string(device) + ".toml"); }

Result<std::optional<ScaleMeasurement>> CameraScaleStore::load(std::string_view device) const {
  if (!safe_file_part(device)) return fail(ErrorKind::Config, "'" + std::string(device) + "' cannot name a camera's scale file");
  const fs::path path = file(device);
  std::error_code ec;
  if (!fs::exists(path, ec)) return std::optional<ScaleMeasurement>{};
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Config, path.string() + ": cannot be read");
  std::ostringstream text;
  text << in.rdbuf();
  auto map = vision::CameraStageMap::from_toml(text.str());
  if (!map) return fail(ErrorKind::Config, path.string() + ": " + map.error().what);
  auto m = describe(*map);
  if (!m) return fail(ErrorKind::Config, path.string() + ": " + m.error().what);
  return std::optional<ScaleMeasurement>(std::move(*m));
}

Result<void> CameraScaleStore::save(std::string_view device, const ScaleMeasurement& m) const {
  if (!safe_file_part(device)) return fail(ErrorKind::Config, "'" + std::string(device) + "' cannot name a camera's scale file");
  const fs::path path = file(device);
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) return fail(ErrorKind::Io, "cannot write " + path.string() + ": " + ec.message());
  const fs::path tmp = fs::path(path).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    // Only the map is read back; the rest is for the reader.
    out << "# The pixel scale of " << device << "'s camera, measured by jogging the stage.\n"
        << "# Used instead of px_per_mm, flip_x and flip_y in cameras.toml. Delete to go back to those.\n"
        << "# px_per_mm = " << num(m.px_per_mm) << "   flip_x = " << (m.flip_x ? "true" : "false")
        << "   flip_y = " << (m.flip_y ? "true" : "false") << "   skew " << num(m.skew_deg, 2) << " deg\n"
        << m.map.to_toml();
    out.flush();
    if (!out) {
      fs::remove(tmp, ec);
      return fail(ErrorKind::Io, "cannot write " + path.string());
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(tmp, ec);
    return fail(ErrorKind::Io, "cannot write " + path.string() + ": " + why);
  }
  return {};
}

Result<void> CameraScaleStore::clear(std::string_view device) const {
  if (!safe_file_part(device)) return fail(ErrorKind::Config, "'" + std::string(device) + "' cannot name a camera's scale file");
  std::error_code ec;
  fs::remove(file(device), ec);
  if (ec) return fail(ErrorKind::Io, "cannot remove " + file(device).string() + ": " + ec.message());
  return {};
}

}  // namespace pychron::laser
