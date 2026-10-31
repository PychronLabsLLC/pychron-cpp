#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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

// One round hot spot of a glow.
struct GlowLobe {
  Vec2 at_mm{};  // relative to the hole's center in a model; stage mm in a GlowScene
  double sigma_mm = 0.3;
  double peak = 0;  // fraction of depth; above 1 saturates
};

// The grain's whole body, glowing evenly: a polygon, brighter than the dark
// around it, soft over edge_mm. Outline relative to the hole's center in a
// model; stage mm once a scene has it.
struct GlowBody {
  std::vector<Vec2> outline_mm;
  double edge_mm = 0.03;
  double peak = 0;  // fraction of depth
};

struct GlowScene {
  int width = 200, height = 200;
  std::uint16_t pixel_depth = 255;
  double px_per_mm = 23.0;
  Vec2 glow_mm{};
  double sigma_mm = 0.3;    // clamped to >= 1e-3 px so a zero width cannot give NaN
  double elongation = 1.0;  // x sigma = elongation * sigma; clamped to >= 1e-3
  double peak = 1.0;        // fraction of depth; >1 saturates
  // Not empty: the glow is the sum of these round Gaussians over background
  // and noise, and glow_mm, sigma_mm, elongation and peak are not used.
  // Truth is then the peak-weighted center of the lobes.
  std::vector<GlowLobe> lobes;
  // Set: the glow is also this body (outline in stage mm), and lobes are cut
  // at its outline. level = background + cover * (body.peak + lobes). Truth
  // weighs the body in at the outline's centroid with body.peak. Unset: as before.
  std::optional<GlowBody> body;
  double background = 0.02, noise = 0.0;
  bool crosshair = false;
  std::uint32_t seed = 1;
};

// Image convention used everywhere: image +x = stage +x, image +y = stage -y,
// scale px_per_mm. `stage_mm` is where the stage is; the scene shifts by
// -stage_mm. Non-positive width/height give an empty frame. Output is deterministic for a given scene and stage position.
std::pair<Frame, Truth> render(const HoleScene& scene, Vec2 stage_mm);
std::pair<Frame, Truth> render(const GlowScene& scene, Vec2 stage_mm);

// One more hole, the scene's hole_mm, drawn into a frame already rendered
// with the same scene and stage position: the darker pixel wins, so a hole
// is dark on the tray and never lightens one already there. Only the pixels
// around the hole are touched (a tray of holes costs no more frames); its
// noise, shadow and glint are not drawn.
void darken_hole(Frame& frame, const HoleScene& scene, Vec2 stage_mm);
// A tray as a camera sees it at 10 to 50x: machined stainless steel, never one
// grey. Cutter marks run nearly one way, over patches, pits and a few
// scratches. `level` is a fraction of depth, row by row, side x side; a tray
// bigger than that is read by reflecting, so it has no seam. The same scale
// and seed give the same steel.
struct MachinedSurface {
  int side = 0;  // px
  std::vector<float> level;
};
MachinedSurface make_machined_surface(double px_per_mm, std::uint32_t seed);

// The frame the scene gives over that steel, with the holes (stage mm) in it,
// the surface fixed to the stage. Light comes in low from one side: a hole's
// floor is never black, part of it is lit nearly as bright as the tray, and
// what stays dark is a crescent against the wall nearest the lamp, whose
// opposite wall catches a bright rim. How much is lit differs hole to hole.
void render_machined(Frame& frame, const HoleScene& scene, const MachinedSurface& surface,
                     std::span<const Vec2> holes_mm, Vec2 stage_mm);

// A cold grain lying in a hole, seen by the lamp and not by its own glow: its
// outline (stage mm) filled at `level`, a fraction of depth brighter than the
// hole and darker than the tray, with a 1 px soft edge. The tray and the hole
// are the scene's; only the scale and the depth are used.
void draw_grain(Frame& frame, const HoleScene& scene, std::span<const Vec2> outline_mm, Vec2 stage_mm,
                double level = 0.5);

}  // namespace pychron::vision
