#include "pychron/vision/synth.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

namespace {

// A body's outline in pixels, with the box outside which nothing is lit.
struct BodyPx {
  std::vector<Vec2> pts;
  double edge_px = 0;
  double peak = 0;
  double x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // box grown by edge_px
  Vec2 centroid{};

  // 1 inside the outline, falling linearly to 0 over edge_px outside it.
  double cover(int x, int y) const {
    if (x < x0 || x > x1 || y < y0 || y > y1) return 0;
    bool in = false;
    double gap2 = std::numeric_limits<double>::infinity();
    const std::size_t n = pts.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
      const Vec2 a = pts[j], b = pts[i];
      if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
      const double ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
      const double t = len2 > 0 ? std::clamp(((x - a.x) * ex + (y - a.y) * ey) / len2, 0.0, 1.0) : 0.0;
      const double dx = x - (a.x + t * ex), dy = y - (a.y + t * ey);
      gap2 = std::min(gap2, dx * dx + dy * dy);
    }
    if (in) return 1.0;
    return clamp01(1.0 - std::sqrt(gap2) / edge_px);
  }
};

BodyPx body_px(const GlowBody& body, int w, int h, Vec2 stage_mm, double px_per_mm) {
  BodyPx b;
  b.edge_px = std::max(body.edge_mm * px_per_mm, 1e-6);
  b.peak = body.peak;
  b.pts.reserve(body.outline_mm.size());
  for (const Vec2 p : body.outline_mm) b.pts.push_back(target_px(w, h, p, stage_mm, px_per_mm));
  b.x0 = b.y0 = std::numeric_limits<double>::infinity();
  b.x1 = b.y1 = -std::numeric_limits<double>::infinity();
  for (const Vec2 p : b.pts) {
    b.x0 = std::min(b.x0, p.x);
    b.x1 = std::max(b.x1, p.x);
    b.y0 = std::min(b.y0, p.y);
    b.y1 = std::max(b.y1, p.y);
  }
  b.x0 -= b.edge_px;
  b.x1 += b.edge_px;
  b.y0 -= b.edge_px;
  b.y1 += b.edge_px;
  // Area centroid; the vertex mean when the polygon has no area.
  double area2 = 0, cx = 0, cy = 0, mx = 0, my = 0;
  const std::size_t n = b.pts.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2 a = b.pts[j], c = b.pts[i];
    const double cross = a.x * c.y - c.x * a.y;
    area2 += cross;
    cx += (a.x + c.x) * cross;
    cy += (a.y + c.y) * cross;
    mx += c.x;
    my += c.y;
  }
  b.centroid = std::abs(area2) > 1e-12 ? Vec2{cx / (3 * area2), cy / (3 * area2)}
                                       : Vec2{mx / static_cast<double>(n), my / static_cast<double>(n)};
  return b;
}

// A glow of several round lobes, and perhaps a body. Truth is their peak-weighted center (the
// plain mean when none is lit), radius the first lobe's sigma.
std::pair<Frame, Truth> render_lobes(const GlowScene& s, Vec2 stage_mm) {
  Frame f = Frame::make(s.width, s.height, s.pixel_depth);
  struct Spot {
    Vec2 at;
    double inv_two_sigma2;
    double peak;
  };
  std::vector<Spot> spots;
  spots.reserve(s.lobes.size());
  double sum_peak = 0, wx = 0, wy = 0, mx = 0, my = 0;
  for (const GlowLobe& l : s.lobes) {
    const Vec2 at = target_px(f.width, f.height, l.at_mm, stage_mm, s.px_per_mm);
    const double sigma = std::max(l.sigma_mm * s.px_per_mm, kMinSigmaPx);
    spots.push_back({at, 1.0 / (2 * sigma * sigma), l.peak});
    sum_peak += l.peak;
    wx += l.peak * at.x;
    wy += l.peak * at.y;
    mx += at.x;
    my += at.y;
  }
  std::optional<BodyPx> body;
  if (s.body && !s.body->outline_mm.empty()) {
    body = body_px(*s.body, f.width, f.height, stage_mm, s.px_per_mm);
    sum_peak += body->peak;
    wx += body->peak * body->centroid.x;
    wy += body->peak * body->centroid.y;
    mx += body->centroid.x;
    my += body->centroid.y;
  }
  const auto n = static_cast<double>(spots.size() + (body ? 1 : 0));
  const Vec2 t = sum_peak > 0 ? Vec2{wx / sum_peak, wy / sum_peak} : Vec2{mx / n, my / n};
  Noise noise(s.seed, s.noise);
  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      double level = s.background;
      if (!body) {
        for (const Spot& k : spots) {
          const double dx = x - k.at.x, dy = y - k.at.y;
          level += k.peak * std::exp(-(dx * dx + dy * dy) * k.inv_two_sigma2);
        }
      } else if (const double cover = body->cover(x, y); cover > 0) {
        double glow = body->peak;
        for (const Spot& k : spots) {
          const double dx = x - k.at.x, dy = y - k.at.y;
          glow += k.peak * std::exp(-(dx * dx + dy * dy) * k.inv_two_sigma2);
        }
        level += cover * glow;
      }
      if (s.crosshair && on_crosshair(x, y, f.width, f.height)) level = std::max(level, 0.6);
      f.at(x, y) = quantise(level + noise.next(), s.pixel_depth);
    }
  }
  double radius = kMinSigmaPx;
  if (!s.lobes.empty()) {
    radius = std::max(s.lobes.front().sigma_mm * s.px_per_mm, kMinSigmaPx);
  } else if (body) {
    // Area-equivalent radius of the outline (shoelace).
    double area2 = 0;
    for (std::size_t i = 0, j = body->pts.size() - 1; i < body->pts.size(); j = i++) {
      area2 += body->pts[j].x * body->pts[i].y - body->pts[i].x * body->pts[j].y;
    }
    radius = std::max(std::sqrt(std::abs(area2) / (2 * std::numbers::pi)), kMinSigmaPx);
  }
  const bool visible = inside(f, t);
  return {std::move(f), Truth{t, radius, visible}};
}

}  // namespace

std::pair<Frame, Truth> render(const GlowScene& s, Vec2 stage_mm) {
  if (!s.lobes.empty() || (s.body && !s.body->outline_mm.empty())) return render_lobes(s, stage_mm);
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
