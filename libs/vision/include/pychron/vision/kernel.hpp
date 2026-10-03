#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pychron/vision/frame.hpp"

namespace pychron::vision {

// Every mask_radius_px / radius_px below describes a disk centred on the frame
// centre; a radius <= 0 means "no mask".

// 3x3 median; edge pixels replicate the border.
Frame median3(const FrameView&);
// Mean over the (2r+1)^2 window clipped to the frame; r <= 0 is a copy and r is
// clamped to max(width, height) (the window is then the whole frame).
Frame box_blur(const FrameView&, int radius);
// Sets pixels outside the disk to `outside`. No-op without a mask.
void apply_disk_mask(Frame&, double radius_px, std::uint16_t outside);
// Otsu threshold over pixels inside the mask: a pixel <= result is background.
// A constant image returns that constant.
std::uint16_t otsu(const FrameView&, double mask_radius_px);
std::uint16_t median_in_mask(const FrameView&, double mask_radius_px);

struct Component {
  int label;
  double area;
  Vec2 centroid;
  Rect bbox;
  double perimeter;
  // Within 1.5 px of the mask circle or of the frame border (a blob cut off by
  // either is truncated, so its shape cannot be trusted).
  bool touches_mask_edge;
  std::vector<Vec2> boundary;
};

// mask: 1 = foreground, row-major w*h. 8-connected; interior holes are filled
// and counted in area/centroid. perimeter is the number of foreground pixels
// with a 4-neighbour background pixel, and boundary holds their centres.
std::vector<Component> components(const std::vector<std::uint8_t>& mask, int w, int h, double mask_radius_px);

struct CircleFit {
  Vec2 center;
  double radius;
  double rms;  // RMS of |p - center| - radius
};
// Algebraic (Kasa) fit. Degenerate input (< 3 points, collinear) yields
// infinite radius and rms rather than a crash.
CircleFit fit_circle(std::span<const Vec2> points);

}  // namespace pychron::vision
