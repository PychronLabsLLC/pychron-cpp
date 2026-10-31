#include "pychron/vision/synth.hpp"

#include <algorithm>
#include <array>
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

// Everything drawn from one generator, the same on every platform: a
// std::uniform_real_distribution is not.
double unit(std::mt19937& rng) { return (static_cast<double>(rng()) + 0.5) / 4294967296.0; }

// Random values with a width of about `radius` px and a standard deviation of 1.
std::vector<float> smooth_noise(std::size_t n, int radius, std::mt19937& rng) {
  std::vector<float> out(n);
  for (float& v : out) v = static_cast<float>(2 * unit(rng) - 1);
  std::vector<float> from(n);
  for (int pass = 0; pass < 3; ++pass) {
    from.swap(out);
    double sum = 0;
    for (int i = -radius; i <= radius; ++i) sum += from[static_cast<std::size_t>(std::abs(i))];
    const int count = 2 * radius + 1;
    const int last = static_cast<int>(n) - 1;
    for (int i = 0; i <= last; ++i) {
      out[static_cast<std::size_t>(i)] = static_cast<float>(sum / count);
      sum += from[static_cast<std::size_t>(std::abs(last - std::abs(last - (i + radius + 1))))];
      sum -= from[static_cast<std::size_t>(std::abs(i - radius))];
    }
  }
  double squares = 0;
  for (const float v : out) squares += static_cast<double>(v) * v;
  const double deviation = std::sqrt(squares / static_cast<double>(n));
  if (deviation > 0) {
    for (float& v : out) v = static_cast<float>(v / deviation);
  }
  return out;
}

// Value noise on a coarse lattice, about -1 to 1.
class Blotches {
 public:
  Blotches(int side, double cell_px, std::mt19937& rng) : cell_(cell_px), cells_(static_cast<int>(side / cell_px) + 3) {
    values_.resize(static_cast<std::size_t>(cells_) * cells_);
    for (float& v : values_) v = static_cast<float>(2 * unit(rng) - 1);
  }
  double at(double x, double y) const {
    const double gx = x / cell_;
    const double gy = y / cell_;
    const int ix = static_cast<int>(gx);
    const int iy = static_cast<int>(gy);
    const double fx = ease(gx - ix);
    const double fy = ease(gy - iy);
    const double top = node(ix, iy) + (node(ix + 1, iy) - node(ix, iy)) * fx;
    const double bottom = node(ix, iy + 1) + (node(ix + 1, iy + 1) - node(ix, iy + 1)) * fx;
    return top + (bottom - top) * fy;
  }

 private:
  static double ease(double t) { return t * t * (3 - 2 * t); }
  double node(int x, int y) const { return values_[static_cast<std::size_t>(y) * cells_ + x]; }
  double cell_;
  int cells_;
  std::vector<float> values_;
};

// i reflected into 0..side-1.
int reflected(std::int64_t i, int side) {
  const std::int64_t period = 2 * static_cast<std::int64_t>(side);
  const std::int64_t at = ((i % period) + period) % period;
  return static_cast<int>(at < side ? at : period - 1 - at);
}

double smoothstep(double edge0, double edge1, double v) {
  const double t = clamp01((v - edge0) / (edge1 - edge0));
  return t * t * (3 - 2 * t);
}

}  // namespace

MachinedSurface make_machined_surface(double px_per_mm, std::uint32_t seed) {
  constexpr int kSide = 1024;
  constexpr double kTray = 0.56;
  constexpr double kCutterAngle = 0.14;  // radians off the frame's y axis
  const double scale = std::isfinite(px_per_mm) && px_per_mm > 0 ? px_per_mm / 23.0 : 1.0;
  const auto px = [scale](double at_23) { return std::max(1, static_cast<int>(std::lround(at_23 * scale))); };
  std::mt19937 rng(seed);

  // Cutter marks: one row of values, swept along the cutter's way.
  const std::size_t across = 3 * kSide;
  std::vector<float> marks(across, 0.0F);
  const std::array<std::pair<int, double>, 3> widths{{{px(1), 0.040}, {px(3), 0.030}, {px(10), 0.016}}};
  for (const auto& [radius, amount] : widths) {
    const std::vector<float> layer = smooth_noise(across, radius, rng);
    for (std::size_t i = 0; i < across; ++i) marks[i] += static_cast<float>(amount) * layer[i];
  }
  const Blotches wobble(kSide, 120.0 * scale, rng);
  const Blotches depth(kSide, 60.0 * scale, rng);
  const Blotches wide(kSide, 70.0 * scale, rng);
  const Blotches close(kSide, 14.0 * scale, rng);

  MachinedSurface out;
  out.side = kSide;
  out.level.resize(static_cast<std::size_t>(kSide) * kSide);
  const double c = std::cos(kCutterAngle);
  const double s = std::sin(kCutterAngle);
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      const double u = x * c + y * s + 10.0 * scale * wobble.at(x, y) + kSide;
      const auto mark = marks[std::min(across - 1, static_cast<std::size_t>(std::max(u, 0.0)))];
      const double level = kTray + 0.05 * wide.at(x, y) + 0.022 * close.at(x, y) +
                           static_cast<double>(mark) * (0.8 + 0.6 * depth.at(x, y)) + 0.012 * (2 * unit(rng) - 1);
      out.level[static_cast<std::size_t>(y) * kSide + x] = static_cast<float>(level);
    }
  }
  const auto mark_px = [&out](int x, int y, double by) {
    if (x < 0 || y < 0 || x >= out.side || y >= out.side) return;
    out.level[static_cast<std::size_t>(y) * out.side + x] += static_cast<float>(by);
  };
  // Pits, a little darker than the marks they break.
  for (int i = 0; i < kSide * kSide / 5000; ++i) {
    const int x = static_cast<int>(unit(rng) * kSide);
    const int y = static_cast<int>(unit(rng) * kSide);
    const int r = static_cast<int>(unit(rng) * px(1.5));
    const double dark = -0.04 - 0.1 * unit(rng);
    for (int dy = -r; dy <= r; ++dy) {
      for (int dx = -r; dx <= r; ++dx) {
        if (dx * dx + dy * dy <= r * r) mark_px(x + dx, y + dy, dark);
      }
    }
  }
  // Scratches, each a thin bright line with its own heading.
  for (int i = 0; i < kSide / 12; ++i) {
    double x = unit(rng) * kSide;
    double y = unit(rng) * kSide;
    const double heading = unit(rng) * 2 * std::numbers::pi;
    const int length = static_cast<int>((20 + unit(rng) * 120) * scale);
    for (int step = 0; step < length; ++step) {
      mark_px(static_cast<int>(x), static_cast<int>(y), 0.05);
      x += std::cos(heading);
      y += std::sin(heading);
    }
  }
  return out;
}

void render_machined(Frame& f, const HoleScene& s, const MachinedSurface& surface, std::span<const Vec2> holes_mm,
                     Vec2 stage_mm) {
  if (f.width <= 0 || f.height <= 0 || f.data.size() < static_cast<std::size_t>(f.width) * f.height) return;
  if (surface.side <= 0 || !std::isfinite(s.px_per_mm) || !std::isfinite(stage_mm.x) || !std::isfinite(stage_mm.y)) return;
  // Where the lamp's light falls: the lit part of a floor lies this way.
  constexpr Vec2 kLight{0.6, 0.8};
  constexpr double kShadow = 0.17, kLitFloor = 0.47;
  const double radius = s.hole_radius_mm * s.px_per_mm;

  struct Lit {
    Vec2 at;       // the hole, px
    Vec2 patch;    // the lit part's center, px
  };
  std::vector<Lit> holes;
  holes.reserve(holes_mm.size());
  for (const Vec2 hole : holes_mm) {
    const Vec2 at = target_px(f.width, f.height, hole, stage_mm, s.px_per_mm);
    if (!std::isfinite(at.x) || !std::isfinite(at.y)) continue;
    // Each hole's walls and depth are its own: the same every look.
    const double drawn = std::sin(hole.x * 12.9898 + hole.y * 78.233) * 43758.5453;
    const double shift = (0.2 + 0.35 * (drawn - std::floor(drawn))) * radius;
    holes.push_back({at, {at.x + kLight.x * shift, at.y + kLight.y * shift}});
  }

  Noise noise(s.seed, s.noise);
  const Vec2 center = image_center(f.width, f.height);
  const double far = std::hypot(center.x, center.y);
  const auto world_x = static_cast<std::int64_t>(std::llround(stage_mm.x * s.px_per_mm));
  const auto world_y = static_cast<std::int64_t>(std::llround(-stage_mm.y * s.px_per_mm));
  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      const int tx = reflected(world_x + x, surface.side);
      const int ty = reflected(world_y + y, surface.side);
      double level = surface.level[static_cast<std::size_t>(ty) * surface.side + tx];
      for (const Lit& hole : holes) {
        const double dx = x - hole.at.x;
        const double dy = y - hole.at.y;
        const double d = std::hypot(dx, dy);
        if (d > radius + 3) continue;
        const double lit = 1 - smoothstep(radius * 0.92, radius * 1.08, std::hypot(x - hole.patch.x, y - hole.patch.y));
        const double floor = kShadow + (kLitFloor - kShadow) * lit + 0.35 * (level - 0.56);
        const double inside = clamp01(radius - d + 0.5);
        level += (floor - level) * inside;
        // The chamfer: bright where it faces the lamp, a thin line.
        if (d > 0) {
          const double facing = std::max(0.0, (dx * kLight.x + dy * kLight.y) / d);
          level += 0.22 * facing * clamp01(1 - std::abs(d - radius) / 2);
        }
      }
      // The lamp lights the middle of the view best, and one side more.
      const double r = std::hypot(x - center.x, y - center.y) / std::max(far, 1.0);
      const double lamp = 1 - 0.14 * r * r + 0.05 * ((x - center.x) * kLight.x + (y - center.y) * kLight.y) / std::max(far, 1.0);
      f.at(x, y) = quantise(level * lamp + noise.next(), s.pixel_depth);
    }
  }
}

void draw_grain(Frame& f, const HoleScene& s, std::span<const Vec2> outline_mm, Vec2 stage_mm, double level) {
  if (f.width <= 0 || f.height <= 0 || f.data.size() < static_cast<std::size_t>(f.width) * f.height) return;
  if (outline_mm.size() < 3) return;
  std::vector<Vec2> at;
  at.reserve(outline_mm.size());
  for (const Vec2 p : outline_mm) {
    const Vec2 px = target_px(f.width, f.height, p, stage_mm, s.px_per_mm);
    if (!std::isfinite(px.x) || !std::isfinite(px.y)) return;
    at.push_back(px);
  }
  const auto [left, right] = std::minmax_element(at.begin(), at.end(), [](Vec2 a, Vec2 b) { return a.x < b.x; });
  const auto [top, bottom] = std::minmax_element(at.begin(), at.end(), [](Vec2 a, Vec2 b) { return a.y < b.y; });
  const int x0 = std::max(0, static_cast<int>(std::floor(left->x - 1)));
  const int x1 = std::min(f.width - 1, static_cast<int>(std::ceil(right->x + 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(top->y - 1)));
  const int y1 = std::min(f.height - 1, static_cast<int>(std::ceil(bottom->y + 1)));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      // Even-odd crossings say which side; the nearest edge how far from it.
      bool in = false;
      double nearest = std::numeric_limits<double>::infinity();
      for (std::size_t i = 0, j = at.size() - 1; i < at.size(); j = i++) {
        const Vec2 a = at[j];
        const Vec2 b = at[i];
        if ((a.y > y) != (b.y > y) && x < a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y)) in = !in;
        const Vec2 ab{b.x - a.x, b.y - a.y};
        const double squared = ab.x * ab.x + ab.y * ab.y;
        const double along = squared > 0 ? clamp01(((x - a.x) * ab.x + (y - a.y) * ab.y) / squared) : 0.0;
        nearest = std::min(nearest, std::hypot(x - (a.x + along * ab.x), y - (a.y + along * ab.y)));
      }
      const double covered = clamp01((in ? nearest : -nearest) + 0.5);
      if (covered <= 0) continue;
      std::uint16_t& px = f.at(x, y);
      const double under = static_cast<double>(px) / s.pixel_depth;
      px = quantise(under + (level - under) * covered, s.pixel_depth);
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
