#include "pychron/vision/synth.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

namespace pychron::vision {
namespace {

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

Vec2 image_center(int w, int h) { return {(w - 1) / 2.0, (h - 1) / 2.0}; }

// Target position in pixels: +x right, stage +y is image up.
Vec2 target_px(int w, int h, Vec2 target_mm, Vec2 stage_mm, double px_per_mm) {
  const Vec2 c = image_center(w, h);
  return {c.x + (target_mm.x - stage_mm.x) * px_per_mm, c.y - (target_mm.y - stage_mm.y) * px_per_mm};
}

bool inside(const Frame& f, Vec2 p) {
  return p.x >= -0.5 && p.x < f.width - 0.5 && p.y >= -0.5 && p.y < f.height - 0.5;
}

// Gaussian noise from raw mt19937 words and our own Box-Muller: the standard
// distributions are implementation-defined, and frames must match everywhere.
class Noise {
 public:
  Noise(std::uint32_t seed, double sigma) : rng_(seed), sigma_(sigma) {}
  double next() {
    if (sigma_ <= 0) return 0;
    if (have_spare_) {
      have_spare_ = false;
      return spare_ * sigma_;
    }
    const double u1 = (static_cast<double>(rng_()) + 0.5) / 4294967296.0;
    const double u2 = (static_cast<double>(rng_()) + 0.5) / 4294967296.0;
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double a = 2.0 * std::numbers::pi * u2;
    spare_ = r * std::sin(a);
    have_spare_ = true;
    return r * std::cos(a) * sigma_;
  }

 private:
  std::mt19937 rng_;
  double sigma_;
  double spare_ = 0;
  bool have_spare_ = false;
};

// Exactly 1 px wide for every frame size: column/row floor((n-1)/2).
bool on_crosshair(int x, int y, int w, int h) { return x == (w - 1) / 2 || y == (h - 1) / 2; }

// Smallest gaussian width (px) and elongation; 0 would give 0/0 = NaN.
constexpr double kMinSigmaPx = 1e-3;
constexpr double kMinElongation = 1e-3;

std::uint16_t quantise(double frac, std::uint16_t depth) {
  if (!std::isfinite(frac)) return 0;  // a NaN stage position or parameter must not reach the cast
  const double v = std::round(clamp01(frac) * depth);
  return static_cast<std::uint16_t>(v);
}

}  // namespace

std::pair<Frame, Truth> render(const HoleScene& s, Vec2 stage_mm) {
  Frame f = Frame::make(s.width, s.height, s.pixel_depth);
  const Vec2 t = target_px(f.width, f.height, s.hole_mm, stage_mm, s.px_per_mm);
  const double r = s.hole_radius_mm * s.px_per_mm;
  const double pitch = s.pitch_mm * s.px_per_mm;
  Noise noise(s.seed, s.noise);

  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      const double dx = x - t.x, dy = y - t.y;
      // Distance to the target hole, or to the nearest grid node when the
      // neighbours are drawn (the target is one node of the grid).
      double dist = std::hypot(dx, dy);
      if (s.neighbours && pitch > 0) {
        dist = std::hypot(dx - std::round(dx / pitch) * pitch, dy - std::round(dy / pitch) * pitch);
      }
      // 1 px linear edge, symmetric about the true radius.
      const double in_hole = clamp01(r - dist + 0.5);
      double level = s.tray_level + (s.hole_level - s.tray_level) * in_hole;

      if (s.shadow) {
        // Shadow cast onto the tray by the hole rim: a copy of the hole
        // shifted down-right, visible only outside the hole itself.
        const double sd = std::hypot(dx - 0.35 * r, dy - 0.35 * r);
        level *= 1.0 - 0.4 * clamp01(r - sd + 0.5) * (1.0 - in_hole);
      }
      if (s.glint && r > 0) {
        // Specular highlight on the tray rim next to the target hole.
        const double gd = std::hypot(dx + 0.8 * r, dy + 0.8 * r);
        const double gr = 0.25 * r;
        level += (1.0 - level) * clamp01(gr - gd + 0.5);
      }
      if (s.crosshair && on_crosshair(x, y, f.width, f.height)) level = 0.0;

      f.at(x, y) = quantise(level + noise.next(), s.pixel_depth);
    }
  }
  const bool visible = inside(f, t);
  return {std::move(f), Truth{t, r, visible}};
}

void darken_hole(Frame& f, const HoleScene& s, Vec2 stage_mm) {
  if (f.width <= 0 || f.height <= 0 || f.data.size() < static_cast<std::size_t>(f.width) * f.height) return;
  const Vec2 t = target_px(f.width, f.height, s.hole_mm, stage_mm, s.px_per_mm);
  const double r = s.hole_radius_mm * s.px_per_mm;
  if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(r) || r < 0) return;
  // The hole and its 1 px edge; the same edge and levels as render().
  const int x0 = std::max(0, static_cast<int>(std::floor(t.x - r - 1)));
  const int x1 = std::min(f.width - 1, static_cast<int>(std::ceil(t.x + r + 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(t.y - r - 1)));
  const int y1 = std::min(f.height - 1, static_cast<int>(std::ceil(t.y + r + 1)));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const double in_hole = clamp01(r - std::hypot(x - t.x, y - t.y) + 0.5);
      if (in_hole <= 0) continue;
      const double level = s.tray_level + (s.hole_level - s.tray_level) * in_hole;
      std::uint16_t& px = f.at(x, y);
      px = std::min(px, quantise(level, s.pixel_depth));
    }
  }
}

std::pair<Frame, Truth> render(const GlowScene& s, Vec2 stage_mm) {
  Frame f = Frame::make(s.width, s.height, s.pixel_depth);
  const Vec2 t = target_px(f.width, f.height, s.glow_mm, stage_mm, s.px_per_mm);
  const double sigma = std::max(s.sigma_mm * s.px_per_mm, kMinSigmaPx);
  const double sx = std::max(s.elongation, kMinElongation) * sigma;
  Noise noise(s.seed, s.noise);

  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      const double dx = x - t.x, dy = y - t.y;
      const double g = s.peak * std::exp(-(dx * dx / (sx * sx) + dy * dy / (sigma * sigma)) / 2.0);
      double level = s.background + g;
      if (s.crosshair && on_crosshair(x, y, f.width, f.height)) level = std::max(level, 0.6);
      f.at(x, y) = quantise(level + noise.next(), s.pixel_depth);
    }
  }
  const bool visible = inside(f, t);
  return {std::move(f), Truth{t, sigma, visible}};
}

}  // namespace pychron::vision
