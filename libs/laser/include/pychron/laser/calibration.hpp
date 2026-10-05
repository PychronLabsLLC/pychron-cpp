#pragma once

// Where a tray sits on a stage: the transform from the tray's frame to stage
// millimetres, solved from points an operator took by driving the stage to
// known holes (laser system design, section 3.2).
//
//   stage = scale * R(rotation) * map + (cx, cy)
//
//   1 point    a shift; no rotation, scale 1
//   2 points   a shift and a rotation, scale 1 (legacy "Tray": center, right)
//   3 or more  least-squares shift, rotation and scale (legacy "Free")
//
// Stage and map are both millimetres, so a fitted scale far from 1 means a
// wrong hole or a wrong map, and is refused.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/laser/tray_map.hpp"

namespace pychron::laser {

struct StageXY {
  double x = 0;
  double y = 0;
};

// The stage was at (x, y) mm when it was on `hole`.
struct CalibrationPoint {
  std::string hole;
  double x = 0;
  double y = 0;
  friend bool operator==(const CalibrationPoint&, const CalibrationPoint&) = default;
};

struct Transform {
  double cx = 0;
  double cy = 0;
  double rotation = 0;  // radians, counter-clockwise
  double scale = 1;

  StageXY to_stage(double mx, double my) const noexcept;
  StageXY to_map(double sx, double sy) const noexcept;  // the exact inverse
};

struct Solution {
  Transform transform;
  // Root mean square miss of the points, in mm. With two points: how far the
  // second is from where the first and the rotation put it.
  double rms_mm = 0;
  std::size_t points = 0;
};

// A fitted scale further than this from 1 is an error.
inline constexpr double kMaxScaleError = 0.02;

// Config error for no points ("not calibrated"), a hole the map lacks, a hole
// given twice, a coordinate that is not finite, two points at the same stage
// position, or a scale outside the rule above.
Result<Solution> solve(const TrayMap& map, std::span<const CalibrationPoint> points);

// What a solved calibration cannot rule out, as sentences for the operator;
// empty when there is nothing to say. None of these show in rms_mm:
//   - the tray turned more than 45 degrees from the map: either it is, or two
//     holes were exchanged (two exchanged points fit perfectly, half a turn
//     round);
//   - fewer than three points, or all of them on one line: a mirrored axis
//     fits as well, and only a hole off that line can show it.
std::vector<std::string> cautions(const TrayMap& map, std::span<const CalibrationPoint> points,
                                  const Solution& solution);

}  // namespace pychron::laser
