#include "pychron/laser/calibration.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <numbers>
#include <sstream>
#include <vector>

namespace pychron::laser {

namespace {

// Stage positions nearer than this are the same place.
constexpr double kSamePlaceMm = 1e-6;

// Fixed-point text that does not depend on the process locale.
std::string fixed(double value, int places) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(places) << value;
  return out.str();
}

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
    return bad(map, "the points fit a scale of " + fixed(fitted_scale, 3) +
                        "; stage and map are both in mm, so a scale more than 2% from 1 means a wrong hole or map");
  }
  t.scale = fitted_scale;
  const auto center = t.to_stage(pmx, pmy);
  t.cx = qmx - center.x;
  t.cy = qmy - center.y;
  double sum = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto s = t.to_stage(holes[i]->x, holes[i]->y);
    sum += (s.x - points[i].x) * (s.x - points[i].x) + (s.y - points[i].y) * (s.y - points[i].y);
  }
  out.rms_mm = std::sqrt(sum / n);
  return out;
}

std::vector<std::string> cautions(const TrayMap& map, std::span<const CalibrationPoint> points,
                                  const Solution& solution) {
  std::vector<std::string> out;
  const double turned = std::abs(solution.transform.rotation) * 180.0 / std::numbers::pi;
  if (turned > 45.0) {
    out.push_back("the tray is turned " + fixed(turned, 0) +
                  " degrees from its map; if it is not, two of the holes were exchanged");
  }
  // On one line: every point's map position is along the line through the
  // first two.
  bool one_line = true;
  if (points.size() >= 3) {
    const Hole* a = map.find(points[0].hole);
    const Hole* b = map.find(points[1].hole);
    for (std::size_t i = 2; one_line && a != nullptr && b != nullptr && i < points.size(); ++i) {
      const Hole* c = map.find(points[i].hole);
      if (c == nullptr) continue;
      const double cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
      const double size = std::hypot(b->x - a->x, b->y - a->y) * std::hypot(c->x - a->x, c->y - a->y);
      if (std::abs(cross) > 1e-3 * size) one_line = false;
    }
  }
  if (one_line) {
    out.push_back(std::string(points.size() < 3 ? "fewer than three points" : "points on one line") +
                  " cannot show a mirrored axis; check a hole off that line (elctl laser goto)");
  }
  return out;
}

}  // namespace pychron::laser
