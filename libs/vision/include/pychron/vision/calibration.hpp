#pragma once

#include <span>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

// One calibration jog: the stage moved by stage_delta_mm and a fixed target
// shifted by image_delta_px in the image.
struct JogPair {
  Vec2 stage_delta_mm;
  Vec2 image_delta_px;
};

// Maps the image offset of a target (target center minus image center, px) to
// the stage move (mm) that centers it. A wrong sign here makes autocenter and
// dragonfly run away from the target, so the convention is pinned by tests.
struct CameraStageMap {
  double m[2][2] = {{1, 0}, {0, 1}};  // px -> mm, stage frame
  double residual_mm = 0;             // RMS fit error from solve(); 0 otherwise

  Vec2 to_mm(Vec2 px) const;
  bool valid() const;  // finite and |det| > 1e-12

  // Diagonal map of 1/px_per_mm; a flipped axis is negated. The synthetic
  // scene convention (image +y = stage -y) is from_scale(p, false, true).
  static CameraStageMap from_scale(double px_per_mm, bool flip_x, bool flip_y);

  // Least squares over M * image_delta = -stage_delta. Needs two or more pairs
  // whose image deltas span the plane.
  static Result<CameraStageMap> solve(std::span<const JogPair> pairs);

  std::string to_toml() const;
  static Result<CameraStageMap> from_toml(std::string_view text);
};

}  // namespace pychron::vision
