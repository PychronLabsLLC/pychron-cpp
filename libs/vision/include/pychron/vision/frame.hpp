#pragma once

#include <cstdint>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

struct FrameView;

// Pixel coordinates throughout the library: a pixel's centre sits at integer
// (x, y), so the centre of a w x h frame is ((w-1)/2, (h-1)/2).
//
// Pixels are always stored as uint16_t; pixel_depth is the maximum value
// (255, 4095, 65535), so 8-, 12- and 16-bit cameras share one code path.
struct Frame {
  int width = 0, height = 0;
  std::uint16_t pixel_depth = 255;
  TimePoint timestamp{};
  std::uint64_t seq = 0;
  std::vector<std::uint16_t> data;  // row-major, width*height

  static Frame make(int w, int h, std::uint16_t depth, std::uint16_t fill = 0);
  std::uint16_t& at(int x, int y);
  FrameView view() const;
};

// Non-owning window onto pixels (possibly a sub-rectangle: stride may exceed width).
struct FrameView {
  const std::uint16_t* data;
  int width, height, stride;
  std::uint16_t pixel_depth;
  TimePoint timestamp;
  std::uint64_t seq;

  std::uint16_t at(int x, int y) const {
    return data[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)];
  }
};

// Copies the part of `r` that lies inside the frame; fully outside gives an empty frame.
Frame crop(const FrameView& v, Rect r);

// Square of side `side` centred on the frame centre, shifted by `offset` pixels
// (rounded). The rectangle is not clamped to the frame (crop() does that), but
// its corner is limited to +-1e9 so a huge offset cannot overflow int.
Rect centered_rect(const FrameView& v, int side, Vec2 offset_px = {});

}  // namespace pychron::vision
