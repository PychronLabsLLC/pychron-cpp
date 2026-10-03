#include "pychron/vision/calibration.hpp"

#include <charconv>
#include <cmath>
#include <toml++/toml.hpp>

namespace pychron::vision {
namespace {

bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

// Shortest text that parses back to the same double, always a TOML float.
std::string number(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  std::string s(buf, r.ptr);
  if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // 'n' covers inf/nan
  return s;
}

}  // namespace

Vec2 CameraStageMap::to_mm(Vec2 px) const {
  return {m[0][0] * px.x + m[0][1] * px.y, m[1][0] * px.x + m[1][1] * px.y};
}

bool CameraStageMap::valid() const {
  for (const auto& row : m)
    for (double v : row)
      if (!std::isfinite(v)) return false;
  return std::abs(m[0][0] * m[1][1] - m[0][1] * m[1][0]) > 1e-12;
}

CameraStageMap CameraStageMap::from_scale(double px_per_mm, bool flip_x, bool flip_y) {
  CameraStageMap map;
  const double sx = (flip_x ? -1.0 : 1.0) / px_per_mm;
  const double sy = (flip_y ? -1.0 : 1.0) / px_per_mm;
  map.m[0][0] = sx;
  map.m[0][1] = 0;
  map.m[1][0] = 0;
  map.m[1][1] = sy;
  return map;
}

Result<CameraStageMap> CameraStageMap::solve(std::span<const JogPair> pairs) {
  if (pairs.size() < 2) return fail(ErrorKind::Config, "calibration needs at least two jog pairs");
  for (const auto& p : pairs)
    if (!finite(p.stage_delta_mm) || !finite(p.image_delta_px))
      return fail(ErrorKind::Config, "calibration jog pair is not finite");

  // N = sum a a^T (image deltas), B = sum (-s) a^T; M = B N^-1.
  double n00 = 0, n01 = 0, n11 = 0, b00 = 0, b01 = 0, b10 = 0, b11 = 0;
  for (const auto& p : pairs) {
    const Vec2 a = p.image_delta_px;
    const Vec2 t{-p.stage_delta_mm.x, -p.stage_delta_mm.y};
    n00 += a.x * a.x;
    n01 += a.x * a.y;
    n11 += a.y * a.y;
    b00 += t.x * a.x;
    b01 += t.x * a.y;
    b10 += t.y * a.x;
    b11 += t.y * a.y;
  }
  const double det = n00 * n11 - n01 * n01;
  const double trace = n00 + n11;
  // det/trace^2 is scale free (at most 1/4); tiny means the deltas lie on a line.
  if (!(trace > 0) || !(det > 1e-9 * trace * trace))
    return fail(ErrorKind::Config, "calibration jogs are collinear in the image");

  CameraStageMap map;
  map.m[0][0] = (b00 * n11 - b01 * n01) / det;
  map.m[0][1] = (b01 * n00 - b00 * n01) / det;
  map.m[1][0] = (b10 * n11 - b11 * n01) / det;
  map.m[1][1] = (b11 * n00 - b10 * n01) / det;
  if (!map.valid()) return fail(ErrorKind::Config, "calibration produced a singular map");

  double sum = 0;
  for (const auto& p : pairs) {
    const Vec2 e = map.to_mm(p.image_delta_px);
    const double ex = e.x + p.stage_delta_mm.x, ey = e.y + p.stage_delta_mm.y;
    sum += ex * ex + ey * ey;
  }
  map.residual_mm = std::sqrt(sum / static_cast<double>(pairs.size()));
  return map;
}

std::string CameraStageMap::to_toml() const {
  return "m = [[" + number(m[0][0]) + ", " + number(m[0][1]) + "], [" + number(m[1][0]) + ", " +
         number(m[1][1]) + "]]\nresidual_mm = " + number(residual_mm) + "\n";
}

Result<CameraStageMap> CameraStageMap::from_toml(std::string_view text) {
  auto parsed = toml::parse(text);
  if (!parsed) return fail(ErrorKind::Config, std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();

  const auto* rows = root.get("m") ? root.get("m")->as_array() : nullptr;
  if (!rows || rows->size() != 2) return fail(ErrorKind::Config, "m must be a 2x2 array");

  CameraStageMap map;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto* row = (*rows)[i].as_array();
    if (!row || row->size() != 2) return fail(ErrorKind::Config, "m must be a 2x2 array");
    for (std::size_t j = 0; j < 2; ++j) {
      const auto& node = (*row)[j];
      if (!node.is_number()) return fail(ErrorKind::Config, "m entries must be numbers");
      map.m[i][j] = node.value<double>().value_or(0.0);
    }
  }
  if (const auto* r = root.get("residual_mm")) {
    if (!r->is_number()) return fail(ErrorKind::Config, "residual_mm must be a number");
    map.residual_mm = r->value<double>().value_or(0.0);
  }
  if (!map.valid()) return fail(ErrorKind::Config, "m is singular or not finite");
  return map;
}

}  // namespace pychron::vision
