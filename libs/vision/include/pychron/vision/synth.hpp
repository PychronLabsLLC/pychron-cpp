#pragma once

#include <cstdint>
#include <utility>

#include "pychron/vision/frame.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

// Ground truth for a rendered scene. center_px uses the library pixel
// convention (pixel centers at integer coordinates), not a rounded pixel.
struct Truth {
  Vec2 center_px{};
  double radius_px = 0;
  bool visible = false;  // target center lies inside the frame
};

struct HoleScene {
  int width = 200, height = 200;
  std::uint16_t pixel_depth = 255;
  double px_per_mm = 23.0;
  double hole_radius_mm = 0.5;
  double pitch_mm = 2.0;
  Vec2 hole_mm{};  // target hole position, stage frame
  double tray_level = 0.75, hole_level = 0.25;  // fractions of pixel_depth
  double noise = 0.0;  // gaussian sigma, fraction of pixel_depth
  // The glint deliberately intrudes slightly inside the hole edge.
  bool neighbours = false, glint = false, crosshair = false, shadow = false;
  std::uint32_t seed = 1;
};

struct GlowScene {
  int width = 200, height = 200;
  std::uint16_t pixel_depth = 255;
  double px_per_mm = 23.0;
  Vec2 glow_mm{};
  double sigma_mm = 0.3;    // clamped to >= 1e-3 px so a zero width cannot give NaN
  double elongation = 1.0;  // x sigma = elongation * sigma; clamped to >= 1e-3
  double peak = 1.0;        // fraction of depth; >1 saturates
  double background = 0.02, noise = 0.0;
  bool crosshair = false;
  std::uint32_t seed = 1;
};

// Image convention used everywhere: image +x = stage +x, image +y = stage -y,
// scale px_per_mm. `stage_mm` is where the stage is; the scene shifts by
// -stage_mm. Non-positive width/height give an empty frame. Output is deterministic for a given scene and stage position.
std::pair<Frame, Truth> render(const HoleScene& scene, Vec2 stage_mm);
std::pair<Frame, Truth> render(const GlowScene& scene, Vec2 stage_mm);

}  // namespace pychron::vision
