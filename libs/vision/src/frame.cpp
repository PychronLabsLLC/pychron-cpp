#include "pychron/vision/frame.hpp"

#include <algorithm>
#include <cmath>

namespace pychron::vision {

Frame Frame::make(int w, int h, std::uint16_t depth, std::uint16_t fill) {
  Frame f;
  f.width = std::max(w, 0);
  f.height = std::max(h, 0);
  f.pixel_depth = depth;
  f.data.assign(static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height), fill);
  return f;
}

std::uint16_t& Frame::at(int x, int y) {
  return data[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

FrameView Frame::view() const {
  return FrameView{data.data(), width, height, width, pixel_depth, timestamp, seq};
}

Frame crop(const FrameView& v, Rect r) {
  const int x0 = std::max(r.x, 0);
  const int y0 = std::max(r.y, 0);
  // 64-bit so a huge w/h cannot overflow the sum.
  const auto x1 = std::min<long long>(static_cast<long long>(r.x) + r.w, v.width);
  const auto y1 = std::min<long long>(static_cast<long long>(r.y) + r.h, v.height);
  const int w = x1 > x0 ? static_cast<int>(x1 - x0) : 0;
  const int h = y1 > y0 ? static_cast<int>(y1 - y0) : 0;
  Frame out = Frame::make(w, h, v.pixel_depth);
  out.timestamp = v.timestamp;
  out.seq = v.seq;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) out.at(x, y) = v.at(x0 + x, y0 + y);
  return out;
}

namespace {

// An out-of-range double must never reach lround or the int cast (UB, or a
// silent wrap). +-1e9 leaves room for the caller's x + w in int; a non-finite
// value (no meaningful position) is taken as 0.
int to_coord(double v) {
  constexpr double kLimit = 1.0e9;
  if (!std::isfinite(v)) return 0;
  return static_cast<int>(std::lround(std::clamp(v, -kLimit, kLimit)));
}

}  // namespace

Rect centered_rect(const FrameView& v, int side, Vec2 offset_px) {
  const double cx = v.width / 2.0 + offset_px.x;
  const double cy = v.height / 2.0 + offset_px.y;
  return Rect{to_coord(cx - side / 2.0), to_coord(cy - side / 2.0), side, side};
}

}  // namespace pychron::vision
