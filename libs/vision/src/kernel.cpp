#include "pychron/vision/kernel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pychron::vision {

namespace {

std::size_t idx(int x, int y, int w) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); }

struct Disk {
  bool on;
  double cx, cy, r2;
  bool contains(int x, int y) const {
    if (!on) return true;
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r2;
  }
};

Disk make_disk(int w, int h, double radius) {
  return Disk{radius > 0, (w - 1) / 2.0, (h - 1) / 2.0, radius * radius};
}

}  // namespace

Frame median3(const FrameView& v) {
  Frame out = Frame::make(v.width, v.height, v.pixel_depth);
  out.timestamp = v.timestamp;
  out.seq = v.seq;
  for (int y = 0; y < v.height; ++y)
    for (int x = 0; x < v.width; ++x) {
      std::array<std::uint16_t, 9> win{};
      std::size_t n = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          win[n++] = v.at(std::clamp(x + dx, 0, v.width - 1), std::clamp(y + dy, 0, v.height - 1));
      std::nth_element(win.begin(), win.begin() + 4, win.end());
      out.at(x, y) = win[4];
    }
  return out;
}

Frame box_blur(const FrameView& v, int radius) {
  Frame out = Frame::make(v.width, v.height, v.pixel_depth);
  out.timestamp = v.timestamp;
  out.seq = v.seq;
  if (radius <= 0) {
    for (int y = 0; y < v.height; ++y)
      for (int x = 0; x < v.width; ++x) out.at(x, y) = v.at(x, y);
    return out;
  }
  // A window wider than the frame is the whole frame; the clamp also keeps
  // x + radius from overflowing int.
  radius = std::min(radius, std::max(v.width, v.height));
  // Integral image: (w+1)*(h+1) so the window sum is four lookups.
  const int iw = v.width + 1;
  std::vector<std::uint64_t> integral(static_cast<std::size_t>(iw) * static_cast<std::size_t>(v.height + 1), 0);
  for (int y = 0; y < v.height; ++y) {
    std::uint64_t row = 0;
    for (int x = 0; x < v.width; ++x) {
      row += v.at(x, y);
      integral[idx(x + 1, y + 1, iw)] = integral[idx(x + 1, y, iw)] + row;
    }
  }
  for (int y = 0; y < v.height; ++y)
    for (int x = 0; x < v.width; ++x) {
      const int x0 = std::max(x - radius, 0), x1 = std::min(x + radius, v.width - 1) + 1;
      const int y0 = std::max(y - radius, 0), y1 = std::min(y + radius, v.height - 1) + 1;
      const std::uint64_t sum = integral[idx(x1, y1, iw)] + integral[idx(x0, y0, iw)] - integral[idx(x0, y1, iw)] -
                                integral[idx(x1, y0, iw)];
      const auto n = static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0);
      out.at(x, y) = static_cast<std::uint16_t>((sum + n / 2) / n);
    }
  return out;
}

void apply_disk_mask(Frame& f, double radius_px, std::uint16_t outside) {
  const Disk d = make_disk(f.width, f.height, radius_px);
  if (!d.on) return;
  for (int y = 0; y < f.height; ++y)
    for (int x = 0; x < f.width; ++x)
      if (!d.contains(x, y)) f.at(x, y) = outside;
}

namespace {

// Histogram of in-mask pixels; values above pixel_depth are clamped into the last bin.
std::vector<std::uint64_t> masked_histogram(const FrameView& v, double mask_radius_px, std::uint64_t& count) {
  std::vector<std::uint64_t> hist(static_cast<std::size_t>(v.pixel_depth) + 1, 0);
  const Disk d = make_disk(v.width, v.height, mask_radius_px);
  count = 0;
  for (int y = 0; y < v.height; ++y)
    for (int x = 0; x < v.width; ++x)
      if (d.contains(x, y)) {
        ++hist[std::min<std::size_t>(v.at(x, y), v.pixel_depth)];
        ++count;
      }
  return hist;
}

}  // namespace

std::uint16_t otsu(const FrameView& v, double mask_radius_px) {
  std::uint64_t n = 0;
  const auto hist = masked_histogram(v, mask_radius_px, n);
  if (n == 0) return 0;
  double total = 0;
  for (std::size_t t = 0; t < hist.size(); ++t) total += static_cast<double>(t) * static_cast<double>(hist[t]);

  double best = -1, w_b = 0, sum_b = 0;
  std::size_t best_t = 0;
  bool seen = false;
  for (std::size_t t = 0; t < hist.size(); ++t) {
    if (hist[t] == 0) continue;
    if (!seen) {
      best_t = t;  // constant image falls through to its only value
      seen = true;
    }
    w_b += static_cast<double>(hist[t]);
    sum_b += static_cast<double>(t) * static_cast<double>(hist[t]);
    const double w_f = static_cast<double>(n) - w_b;
    if (w_f <= 0) break;
    const double diff = sum_b / w_b - (total - sum_b) / w_f;
    const double between = w_b * w_f * diff * diff;
    if (between > best) {  // strict: the first of a plateau, so the lower edge of the gap
      best = between;
      best_t = t;
    }
  }
  return static_cast<std::uint16_t>(best_t);
}

std::uint16_t median_in_mask(const FrameView& v, double mask_radius_px) {
  std::uint64_t n = 0;
  const auto hist = masked_histogram(v, mask_radius_px, n);
  if (n == 0) return 0;
  const std::uint64_t target = (n + 1) / 2;  // lower median
  std::uint64_t acc = 0;
  for (std::size_t t = 0; t < hist.size(); ++t) {
    acc += hist[t];
    if (acc >= target) return static_cast<std::uint16_t>(t);
  }
  return 0;
}

std::vector<Component> components(const std::vector<std::uint8_t>& mask, int w, int h, double mask_radius_px) {
  std::vector<Component> out;
  if (w <= 0 || h <= 0 || mask.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) return out;
  const std::size_t total = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

  // 1. Label foreground, 8-connected, in raster order of first pixel.
  std::vector<int> label(total, 0);
  std::vector<std::size_t> stack;
  int next = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!mask[idx(x, y, w)] || label[idx(x, y, w)]) continue;
      ++next;
      label[idx(x, y, w)] = next;
      stack.push_back(idx(x, y, w));
      while (!stack.empty()) {
        const std::size_t p = stack.back();
        stack.pop_back();
        const int px = static_cast<int>(p % static_cast<std::size_t>(w));
        const int py = static_cast<int>(p / static_cast<std::size_t>(w));
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = px + dx, ny = py + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const std::size_t q = idx(nx, ny, w);
            if (mask[q] && !label[q]) {
              label[q] = next;
              stack.push_back(q);
            }
          }
      }
    }

  // 2. Fill holes. Background reachable from the border (4-connected, the dual
  // of 8-connected foreground) is outside; the rest is a hole, given to the
  // component above its first raster pixel. That pixel is always part of the
  // hole's outer wall, so nested blobs inside a hole are not mistaken for it.
  std::vector<std::uint8_t> outside(total, 0);
  auto flood = [&](std::size_t seed, auto&& visit) {
    stack.push_back(seed);
    while (!stack.empty()) {
      const std::size_t p = stack.back();
      stack.pop_back();
      const int px = static_cast<int>(p % static_cast<std::size_t>(w));
      const int py = static_cast<int>(p / static_cast<std::size_t>(w));
      static constexpr std::array<std::array<int, 2>, 4> kDirs{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
      for (const auto& d : kDirs) {
        const int nx = px + d[0], ny = py + d[1];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const std::size_t q = idx(nx, ny, w);
        if (label[q] == 0 && visit(q)) stack.push_back(q);
      }
    }
  };
  auto seed_outside = [&](int x, int y) {
    const std::size_t p = idx(x, y, w);
    if (label[p] == 0 && !outside[p]) {
      outside[p] = 1;
      flood(p, [&](std::size_t q) {
        if (outside[q]) return false;
        outside[q] = 1;
        return true;
      });
    }
  };
  for (int x = 0; x < w; ++x) {
    seed_outside(x, 0);
    seed_outside(x, h - 1);
  }
  for (int y = 0; y < h; ++y) {
    seed_outside(0, y);
    seed_outside(w - 1, y);
  }
  std::vector<int> filled = label;
  std::vector<std::uint8_t> visited = outside;
  for (int y = 1; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const std::size_t p = idx(x, y, w);
      if (label[p] != 0 || visited[p]) continue;
      const int owner = label[idx(x, y - 1, w)];
      visited[p] = 1;
      filled[p] = owner;
      flood(p, [&](std::size_t q) {
        if (visited[q]) return false;
        visited[q] = 1;
        filled[q] = owner;
        return true;
      });
    }

  // 3. Measure.
  const Disk disk = make_disk(w, h, mask_radius_px);
  const double edge_r = mask_radius_px - 1.5;
  out.resize(static_cast<std::size_t>(next));
  std::vector<int> minx(out.size(), w), miny(out.size(), h), maxx(out.size(), -1), maxy(out.size(), -1);
  std::vector<double> sx(out.size(), 0), sy(out.size(), 0);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].label = static_cast<int>(i) + 1;
    out[i].area = 0;
    out[i].perimeter = 0;
    out[i].touches_mask_edge = false;
  }
  auto is_fg = [&](int x, int y, int l) { return x >= 0 && y >= 0 && x < w && y < h && filled[idx(x, y, w)] == l; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int l = filled[idx(x, y, w)];
      if (l == 0) continue;
      const auto i = static_cast<std::size_t>(l - 1);
      auto& c = out[i];
      c.area += 1;
      sx[i] += x;
      sy[i] += y;
      minx[i] = std::min(minx[i], x);
      maxx[i] = std::max(maxx[i], x);
      miny[i] = std::min(miny[i], y);
      maxy[i] = std::max(maxy[i], y);
      if (x == 0 || y == 0 || x == w - 1 || y == h - 1) c.touches_mask_edge = true;
      if (disk.on) {
        const double dx = x - disk.cx, dy = y - disk.cy;
        if (std::hypot(dx, dy) >= edge_r) c.touches_mask_edge = true;
      }
      if (!is_fg(x + 1, y, l) || !is_fg(x - 1, y, l) || !is_fg(x, y + 1, l) || !is_fg(x, y - 1, l)) {
        c.perimeter += 1;
        c.boundary.push_back({static_cast<double>(x), static_cast<double>(y)});
      }
    }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].centroid = {sx[i] / out[i].area, sy[i] / out[i].area};
    out[i].bbox = {minx[i], miny[i], maxx[i] - minx[i] + 1, maxy[i] - miny[i] + 1};
  }
  return out;
}

CircleFit fit_circle(std::span<const Vec2> pts) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  CircleFit bad{{0, 0}, inf, inf};
  if (pts.size() < 3) return bad;
  const auto n = static_cast<double>(pts.size());
  double mx = 0, my = 0;
  for (const auto& p : pts) {
    mx += p.x;
    my += p.y;
  }
  mx /= n;
  my /= n;
  bad.center = {mx, my};
  // Centered coordinates keep the normal equations well conditioned; the linear
  // terms then vanish and the system reduces to 2x2.
  double suu = 0, svv = 0, suv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0, sz = 0;
  for (const auto& p : pts) {
    const double u = p.x - mx, v = p.y - my;
    suu += u * u;
    svv += v * v;
    suv += u * v;
    suuu += u * u * u;
    svvv += v * v * v;
    suvv += u * v * v;
    svuu += v * u * u;
    sz += u * u + v * v;
  }
  const double det = suu * svv - suv * suv;
  if (!(det > 1e-9 * suu * svv) || !(suu * svv > 0)) return bad;
  const double ra = suuu + suvv, rb = svvv + svuu;  // = sum(z*u), sum(z*v)
  const double a = (ra * svv - rb * suv) / (2 * det);
  const double b = (rb * suu - ra * suv) / (2 * det);
  const double r2 = sz / n + a * a + b * b;
  if (!(r2 > 0) || !std::isfinite(r2)) return bad;
  const double r = std::sqrt(r2);
  double ss = 0;
  for (const auto& p : pts) {
    const double d = std::hypot(p.x - mx - a, p.y - my - b) - r;
    ss += d * d;
  }
  return CircleFit{{mx + a, my + b}, r, std::sqrt(ss / n)};
}

}  // namespace pychron::vision
