#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>

#include "pychron/vision/finder.hpp"
#include "pychron/vision/kernel.hpp"

namespace pychron::vision {

namespace {

std::size_t idx(int x, int y, int w) {
  return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
}

bool in_disk(int x, int y, int w, int h, double radius) {
  if (radius <= 0) return true;
  const double dx = x - (w - 1) / 2.0, dy = y - (h - 1) / 2.0;
  return dx * dx + dy * dy <= radius * radius;
}

constexpr double kMinGlowArea = 9;
constexpr double kMinHoleCircularity = 0.6;
constexpr double kMinContrast = 0.05;  // of pixel_depth

double circularity_of(const Component& c) {
  return c.perimeter > 0 ? std::min(1.0, 4 * std::numbers::pi * c.area / (c.perimeter * c.perimeter)) : 0.0;
}

// Pixels of the component containing `seed`: 8-connected flood over the foreground mask.
std::vector<std::size_t> flood(const std::vector<std::uint8_t>& mask, int w, int h, Vec2 seed) {
  std::vector<std::size_t> out;
  std::vector<std::uint8_t> seen(mask.size(), 0);
  std::vector<std::pair<int, int>> stack{{static_cast<int>(seed.x), static_cast<int>(seed.y)}};
  seen[idx(stack[0].first, stack[0].second, w)] = 1;
  while (!stack.empty()) {
    const auto [x, y] = stack.back();
    stack.pop_back();
    out.push_back(idx(x, y, w));
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        const int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const auto q = idx(nx, ny, w);
        if (!mask[q] || seen[q]) continue;
        seen[q] = 1;
        stack.emplace_back(nx, ny);
      }
  }
  return out;
}

}  // namespace

double saturation(const Target& t) { return t.score; }

std::vector<Target> SimpleFinder::find(const FrameView& view, const FinderParams& p, FinderDebug* debug) {
  std::vector<Target> out;
  const int w = view.width, h = view.height;
  if (debug) {
    *debug = FinderDebug{};
    debug->width = std::max(w, 0);
    debug->height = std::max(h, 0);
  }
  if (w <= 0 || h <= 0 || view.data == nullptr) return out;
  // Non-finite parameters have no meaning (a NaN would also slip through
  // std::clamp and the casts below), so they find nothing.
  if (!std::isfinite(p.expected_radius_px) || !std::isfinite(p.radius_tol) || !std::isfinite(p.mask_radius_px) ||
      !std::isfinite(p.glow_fraction) || view.pixel_depth == 0)
    return out;

  const double depth = view.pixel_depth;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::vector<std::uint8_t> mask(n, 0);
  if (debug) debug->mask = mask;

  // Median removes one-pixel overlays; blur steadies the threshold against noise.
  // The mask is applied when thresholding, so pixels outside it never become foreground.
  const Frame med = median3(view);
  // Clamp as a double: a huge radius must not reach lround. A window wider than
  // the frame is the whole frame, so max(w, h) loses nothing.
  const int radius = static_cast<int>(std::lround(std::clamp(p.expected_radius_px / 8.0, 1.0, static_cast<double>(std::max(w, h)))));
  const Frame blurred = box_blur(med.view(), radius);
  const FrameView bv = blurred.view();
  const bool hole = p.mode == FinderMode::Hole;

  std::uint16_t threshold = 0;
  double floor_v = 0;  // glow background level
  if (hole) {
    threshold = otsu(bv, p.mask_radius_px);
    double lo_sum = 0, hi_sum = 0;
    std::size_t lo_n = 0, hi_n = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (in_disk(x, y, w, h, p.mask_radius_px)) {
          const double v = bv.at(x, y);
          if (v <= threshold) {
            lo_sum += v;
            ++lo_n;
          } else {
            hi_sum += v;
            ++hi_n;
          }
        }
    if (lo_n == 0 || hi_n == 0 || hi_sum / static_cast<double>(hi_n) - lo_sum / static_cast<double>(lo_n) < kMinContrast * depth) {
      if (debug) debug->threshold = threshold;
      return out;
    }
  } else {
    floor_v = median_in_mask(bv, p.mask_radius_px);
    double max_v = 0;
    bool any = false;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (in_disk(x, y, w, h, p.mask_radius_px)) {
          max_v = any ? std::max<double>(max_v, bv.at(x, y)) : bv.at(x, y);
          any = true;
        }
    const double frac = std::clamp(p.glow_fraction, 0.0, 1.0);  // keeps the cast below in range
    threshold = static_cast<std::uint16_t>(std::lround(floor_v + frac * (max_v - floor_v)));
    if (!any || max_v - floor_v < kMinContrast * depth) {
      if (debug) debug->threshold = threshold;
      return out;
    }
  }
  if (hole) {
    // A shadow on the tray is darker than the tray but lighter than the hole, so
    // Otsu puts it on the dark side and it fuses with the hole. Pull the
    // threshold halfway from Otsu's split down to the hole's own level (the mode
    // of the dark class) so only pixels close to the hole level stay foreground.
    // Assumes the hole is the largest mass in the dark class: a shadow larger
    // than the hole puts the mode on the shadow and the rule degrades to plain
    // Otsu. Pulling the threshold down also biases the reported radius_px
    // slightly low, by an amount that grows with the blur radius.
    constexpr int kBins = 256;
    std::array<std::uint64_t, kBins> hist{};
    const double bin_w = (depth + 1) / kBins;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (in_disk(x, y, w, h, p.mask_radius_px) && bv.at(x, y) <= threshold)
          ++hist[std::min<std::size_t>(static_cast<std::size_t>(bv.at(x, y) / bin_w), kBins - 1)];
    const auto peak = static_cast<double>(std::max_element(hist.begin(), hist.end()) - hist.begin());
    const double hole_level = (peak + 0.5) * bin_w;
    if (hole_level < threshold) threshold = static_cast<std::uint16_t>(std::lround(hole_level + 0.5 * (threshold - hole_level)));
  }
  if (debug) debug->threshold = threshold;

  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!in_disk(x, y, w, h, p.mask_radius_px)) continue;
      const bool fg = hole ? bv.at(x, y) <= threshold : bv.at(x, y) >= threshold;
      mask[idx(x, y, w)] = fg ? 1 : 0;
    }
  if (debug) debug->mask = mask;

  const auto comps = components(mask, w, h, p.mask_radius_px);
  if (debug) debug->components = static_cast<int>(comps.size());

  const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;
  struct Ranked {
    Target t;
    double key;  // lower sorts first
  };
  std::vector<Ranked> ranked;

  for (const Component& c : comps) {
    if (c.area < kMinGlowArea) {
      if (debug) ++debug->rejected_area;
      continue;
    }
    const double eq_r = std::sqrt(c.area / std::numbers::pi);
    const double circ = circularity_of(c);

    if (hole) {
      if (c.touches_mask_edge) {
        if (debug) ++debug->rejected_edge;
        continue;
      }
      double match = 1;
      if (p.expected_radius_px > 0) {
        const double lo = p.expected_radius_px * (1 - p.radius_tol), hi = p.expected_radius_px * (1 + p.radius_tol);
        if (eq_r < lo || eq_r > hi) {
          if (debug) ++debug->rejected_radius;
          continue;
        }
        match = std::max(0.0, 1 - std::abs(eq_r - p.expected_radius_px) / (p.expected_radius_px * p.radius_tol));
      }
      if (circ < kMinHoleCircularity) {
        if (debug) ++debug->rejected_circularity;
        continue;
      }
      // A shadowed rim pulls the area centroid to the lit side; the boundary fit does not follow it.
      Target t;
      t.center_px = c.centroid;
      t.radius_px = eq_r;
      const CircleFit fit = fit_circle(c.boundary);
      if (std::isfinite(fit.radius) && std::isfinite(fit.rms) && fit.rms <= 0.2 * fit.radius) {
        t.center_px = fit.center;
        t.radius_px = fit.radius + 0.5;  // boundary pixel centers sit half a pixel inside the edge
      }
      t.area_px = c.area;
      t.circularity = circ;
      t.score = circ * match;
      ranked.push_back({t, std::hypot(c.centroid.x - cx, c.centroid.y - cy)});
    } else {
      const auto pixels = flood(mask, w, h, c.boundary.front());
      double sum = 0, wsum = 0, wx = 0, wy = 0;
      for (const std::size_t q : pixels) {
        const int x = static_cast<int>(q % static_cast<std::size_t>(w)), y = static_cast<int>(q / static_cast<std::size_t>(w));
        const double v = view.at(x, y);
        sum += v;
        const double wt = std::max(0.0, v - floor_v);
        wsum += wt;
        wx += wt * x;
        wy += wt * y;
      }
      Target t;
      t.center_px = wsum > 0 ? Vec2{wx / wsum, wy / wsum} : c.centroid;
      t.radius_px = eq_r;
      t.area_px = c.area;
      t.circularity = circ;
      t.score = std::clamp(sum / static_cast<double>(pixels.size()) / depth, 0.0, 1.0);
      ranked.push_back({t, -sum});
    }
  }

  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.key < b.key; });
  for (const Ranked& r : ranked) out.push_back(r.t);
  return out;
}

}  // namespace pychron::vision
