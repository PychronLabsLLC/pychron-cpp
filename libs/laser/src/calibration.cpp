#include "pychron/laser/calibration.hpp"

#include <cmath>
#include <vector>

namespace pychron::laser {

namespace {

// Stage positions nearer than this are the same place.
constexpr double kSamePlaceMm = 1e-6;

Unexpected<Error> bad(const TrayMap& map, const std::string& what) {
  return fail(ErrorKind::Config, "calibration of tray " + map.name() + ": " + what);
}

}  // namespace

StageXY Transform::to_stage(double mx, double my) const noexcept {
  const double c = std::cos(rotation);
  const double s = std::sin(rotation);
  return {scale * (c * mx - s * my) + cx, scale * (s * mx + c * my) + cy};
}

StageXY Transform::to_map(double sx, double sy) const noexcept {
  const double c = std::cos(rotation);
  const double s = std::sin(rotation);
  const double dx = sx - cx;
  const double dy = sy - cy;
  return {(c * dx + s * dy) / scale, (-s * dx + c * dy) / scale};
}

Result<Solution> solve(const TrayMap& map, std::span<const CalibrationPoint> points) {
  if (points.empty()) return bad(map, "not calibrated (no points)");

  std::vector<const Hole*> holes;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto& p = points[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      return bad(map, "the stage position at hole '" + p.hole + "' is not finite");
    }
    const Hole* hole = map.find(p.hole);
    if (hole == nullptr) return bad(map, "hole '" + p.hole + "' is not on the map");
    for (std::size_t j = 0; j < i; ++j) {
      if (points[j].hole == p.hole) return bad(map, "hole '" + p.hole + "' is given twice");
      if (std::hypot(points[j].x - p.x, points[j].y - p.y) < kSamePlaceMm) {
        return bad(map, "holes '" + points[j].hole + "' and '" + p.hole + "' have the same stage position");
      }
      if (std::hypot(holes[j]->x - hole->x, holes[j]->y - hole->y) < kSamePlaceMm) {
        return bad(map, "holes '" + points[j].hole + "' and '" + p.hole + "' are at the same place on the map");
      }
    }
    holes.push_back(hole);
  }

  Solution out;
  out.points = points.size();
  Transform& t = out.transform;

  if (points.size() == 1) {
    t.cx = points[0].x - holes[0]->x;
    t.cy = points[0].y - holes[0]->y;
    return out;
  }

  // Centroids, then the rotation that best turns the map's vectors onto the
  // stage's: atan2 of the summed cross and dot products.
  const double n = static_cast<double>(points.size());
  double pmx = 0, pmy = 0, qmx = 0, qmy = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    pmx += holes[i]->x / n;
    pmy += holes[i]->y / n;
    qmx += points[i].x / n;
    qmy += points[i].y / n;
  }
  double dot = 0, cross = 0, pp = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double px = holes[i]->x - pmx, py = holes[i]->y - pmy;
    const double qx = points[i].x - qmx, qy = points[i].y - qmy;
    dot += px * qx + py * qy;
    cross += px * qy - py * qx;
    pp += px * px + py * py;
  }
  const double fitted_scale = std::hypot(dot, cross) / pp;  // pp > 0: the holes are distinct places
  t.rotation = std::atan2(cross, dot);

  if (points.size() == 2) {
    // Scale stays 1 and the first point is exact; a wrong distance between
    // the two shows as the miss at the second.
    const auto turned = t.to_stage(holes[0]->x, holes[0]->y);
    t.cx = points[0].x - turned.x;
    t.cy = points[0].y - turned.y;
    const auto second = t.to_stage(holes[1]->x, holes[1]->y);
    out.rms_mm = std::hypot(second.x - points[1].x, second.y - points[1].y);
    return out;
  }

  if (!(std::abs(fitted_scale - 1.0) <= kMaxScaleError)) {
    return bad(map, "the points fit a scale of " + std::to_string(fitted_scale) +
                        "; stage and map are both in mm, so a scale more than 2% from 1 means a wrong hole or map");
  }
  t.scale = fitted_scale;
  const auto centre = t.to_stage(pmx, pmy);
  t.cx = qmx - centre.x;
  t.cy = qmy - centre.y;
  double sum = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto s = t.to_stage(holes[i]->x, holes[i]->y);
    sum += (s.x - points[i].x) * (s.x - points[i].x) + (s.y - points[i].y) * (s.y - points[i].y);
  }
  out.rms_mm = std::sqrt(sum / n);
  return out;
}

}  // namespace pychron::laser
