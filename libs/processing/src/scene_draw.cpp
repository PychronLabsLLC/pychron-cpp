// A scene as a list of paths and texts on a page (data publishing design,
// section 5.6).

#include "pychron/processing/scene_draw.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>

namespace pychron::processing {

namespace {

using Kind = PathOp::Kind;

constexpr double kKappa = 0.5522847498307936;  // a quarter circle as one cubic Bezier
constexpr double kSqrtHalf = 0.7071067811865476;
constexpr double kSqrt3Half = 0.8660254037844386;
constexpr double kTickLength = 5.0;
constexpr double kTickGap = 5.0;
constexpr double kOuter = 8.0;
constexpr double kLineHeight = 1.2;
constexpr Color kBlack{0, 0, 0, 255};
constexpr Color kGrid{225, 225, 225, 255};
constexpr Color kOverlay{255, 255, 255, 200};
constexpr Color kExcludedLine{150, 150, 150, 255};

bool finite(double v) { return std::isfinite(v); }

struct Builder {
  DrawList& list;
  const SceneStyle& style;

  void path(DrawPath p) {
    if (!p.ops.empty()) list.items.emplace_back(std::move(p));
  }
  void text(DrawText t) {
    if (!t.text.empty()) list.items.emplace_back(std::move(t));
  }
};

PathOp move(double x, double y) { return {Kind::Move, {{{x, y}, {}, {}}}}; }
PathOp line(double x, double y) { return {Kind::Line, {{{x, y}, {}, {}}}}; }
PathOp curve(Pt a, Pt b, Pt c) { return {Kind::Curve, {{a, b, c}}}; }
PathOp close() { return {Kind::Close, {}}; }

void rect(std::vector<PathOp>& ops, double x0, double y0, double x1, double y1) {
  ops.push_back(move(x0, y0));
  ops.push_back(line(x1, y0));
  ops.push_back(line(x1, y1));
  ops.push_back(line(x0, y1));
  ops.push_back(close());
}

// The unit circle as four cubic arcs, through an affine map (u, v) -> point.
template <class Map>
std::vector<PathOp> circle(Map&& at) {
  const Pt p0 = at(1, 0), p1 = at(0, 1), p2 = at(-1, 0), p3 = at(0, -1);
  return {move(p0.x, p0.y),
          curve(at(1, kKappa), at(kKappa, 1), p1),
          curve(at(-kKappa, 1), at(-1, kKappa), p2),
          curve(at(-1, -kKappa), at(-kKappa, -1), p3),
          curve(at(kKappa, -1), at(1, -kKappa), p0),
          close()};
}

DrawPath marker_path(const MarkerStyle& m, Pt c, DrawRole role, int clip) {
  DrawPath p;
  p.role = role;
  p.clip = clip;
  p.stroke = m.color;
  p.width = 1.2;
  const double r = m.size / 2;
  bool fillable = true;
  switch (m.shape) {
    case MarkerShape::Circle:
      p.ops = circle([&](double u, double v) { return Pt{c.x + r * u, c.y + r * v}; });
      break;
    case MarkerShape::Square:
      rect(p.ops, c.x - r, c.y - r, c.x + r, c.y + r);
      break;
    case MarkerShape::Diamond:
      p.ops = {move(c.x, c.y - r), line(c.x + r, c.y), line(c.x, c.y + r), line(c.x - r, c.y), close()};
      break;
    case MarkerShape::Triangle:
      p.ops = {move(c.x, c.y - r), line(c.x + r * kSqrt3Half, c.y + r / 2), line(c.x - r * kSqrt3Half, c.y + r / 2),
               close()};
      break;
    case MarkerShape::Cross:
    case MarkerShape::Plus:
    case MarkerShape::Star: {
      fillable = false;
      const double d = r * kSqrtHalf;
      if (m.shape != MarkerShape::Plus) {
        p.ops.insert(p.ops.end(), {move(c.x - d, c.y - d), line(c.x + d, c.y + d), move(c.x - d, c.y + d),
                                   line(c.x + d, c.y - d)});
      }
      if (m.shape != MarkerShape::Cross) {
        p.ops.insert(p.ops.end(), {move(c.x - r, c.y), line(c.x + r, c.y), move(c.x, c.y - r), line(c.x, c.y + r)});
      }
      break;
    }
  }
  if (fillable && m.filled) p.fill = m.color;
  return p;
}

// A panel's mapping, leaving out what cannot be placed.
struct Mapper {
  const PanelLayout& panel;
  std::optional<Pt> operator()(double x, double y) const {
    if (!finite(x) || !finite(y) || (panel.log_y && y <= 0)) return std::nullopt;
    const Pt p{map_x(panel, x), map_y(panel, y)};
    if (!finite(p.x) || !finite(p.y)) return std::nullopt;
    return p;
  }
};

Color darker(Color c) {  // Qt's darker(150), near enough: each channel / 1.5
  const auto d = [](std::uint8_t v) { return static_cast<std::uint8_t>(v * 2 / 3); };
  return {d(c.r), d(c.g), d(c.b), c.a};
}

void draw_points(Builder& b, const PointLayer& pts, const Mapper& map, int clip) {
  for (const bool excluded : {false, true}) {
    if (excluded && !pts.show_excluded) continue;
    const MarkerStyle& m = excluded ? pts.excluded_marker : pts.marker;
    for (std::size_t i = 0; i < pts.x.size() && i < pts.y.size(); ++i) {
      const bool ex = i < pts.excluded.size() && pts.excluded[i];
      if (ex != excluded) continue;
      const auto c = map(pts.x[i], pts.y[i]);
      if (!c) continue;
      const auto bar = [&](double x0, double y0, double x1, double y1) {
        const auto a = map(x0, y0), z = map(x1, y1);
        if (!a || !z) return;
        DrawPath p;
        p.role = DrawRole::ErrorBar;
        p.clip = clip;
        p.stroke = m.color;
        p.width = 1.0;
        p.ops = {move(a->x, a->y), line(z->x, z->y)};
        b.path(std::move(p));
      };
      if (i < pts.y_err.size() && finite(pts.y_err[i]) && pts.y_err[i] > 0) {
        // On a log axis a bar reaching zero or below runs to the bottom of the panel.
        double low = pts.y[i] - pts.y_err[i];
        if (map.panel.log_y && low <= 0) low = map.panel.y.min;
        bar(pts.x[i], low, pts.x[i], pts.y[i] + pts.y_err[i]);
      }
      if (i < pts.x_err.size() && finite(pts.x_err[i]) && pts.x_err[i] > 0)
        bar(pts.x[i] - pts.x_err[i], pts.y[i], pts.x[i] + pts.x_err[i], pts.y[i]);
      b.path(marker_path(m, *c, DrawRole::Marker, clip));
    }
  }
}

void draw_line(Builder& b, const LineLayer& l, const Mapper& map, int clip) {
  DrawPath p;
  p.role = DrawRole::Data;
  p.clip = clip;
  p.stroke = l.style.color;
  p.width = l.style.width;
  p.dash = l.style.dash;
  bool open = false;
  for (std::size_t i = 0; i < l.x.size() && i < l.y.size(); ++i) {
    const auto pt = map(l.x[i], l.y[i]);
    if (!pt) {
      open = false;
      continue;
    }
    p.ops.push_back(open ? line(pt->x, pt->y) : move(pt->x, pt->y));
    open = true;
  }
  b.path(std::move(p));
}

void draw_band(Builder& b, const BandLayer& band, const Mapper& map, int clip) {
  std::vector<Pt> low, high;
  for (std::size_t i = 0; i < band.x.size() && i < band.low.size() && i < band.high.size(); ++i) {
    const auto lo = map(band.x[i], band.low[i]), hi = map(band.x[i], band.high[i]);
    if (!lo || !hi) continue;
    low.push_back(*lo);
    high.push_back(*hi);
  }
  if (low.size() < 2) return;
  DrawPath p;
  p.role = DrawRole::Band;
  p.clip = clip;
  p.fill = band.fill;
  p.ops.push_back(move(low[0].x, low[0].y));
  for (std::size_t i = 1; i < low.size(); ++i) p.ops.push_back(line(low[i].x, low[i].y));
  for (std::size_t i = high.size(); i-- > 0;) p.ops.push_back(line(high[i].x, high[i].y));
  p.ops.push_back(close());
  b.path(std::move(p));
}

void draw_steps(Builder& b, const StepLayer& s, const Mapper& map, int clip) {
  for (std::size_t i = 0; i < s.x0.size() && i < s.x1.size() && i < s.y.size(); ++i) {
    const double e = i < s.y_err.size() && finite(s.y_err[i]) ? s.y_err[i] : 0.0;
    // On a log axis a box reaching zero or below runs to the bottom of the panel.
    double low = s.y[i] - e;
    if (map.panel.log_y && low <= 0) low = map.panel.y.min;
    const auto a = map(s.x0[i], s.y[i] + e), z = map(s.x1[i], low);
    if (!a || !z) continue;
    const bool ex = i < s.excluded.size() && s.excluded[i];
    const bool hl = i < s.highlighted.size() && s.highlighted[i];
    DrawPath p;
    p.role = DrawRole::Step;
    p.clip = clip;
    Color fill = ex ? s.excluded_fill : s.fill;
    if (!ex && s.dim_others && !hl) fill.a = static_cast<std::uint8_t>(fill.a / 3);
    p.fill = fill;
    p.stroke = ex ? darker(s.excluded_fill) : s.line;
    p.width = ex ? 0.8 : 1.0;
    p.dash = ex ? LineDash::Dash : LineDash::Solid;
    rect(p.ops, a->x, a->y, z->x, z->y);
    b.path(std::move(p));
    if (i < s.labels.size() && !s.labels[i].empty()) {
      DrawText t;
      t.role = TextRole::StepLabel;
      t.clip = clip;
      t.at = {(a->x + z->x) / 2, std::min(a->y, z->y) - 2};
      t.anchor = TextAnchor::Middle;
      t.size = b.style.fonts.annotation;
      t.text = s.labels[i];
      b.text(std::move(t));
    }
  }
}

void draw_ellipses(Builder& b, const EllipseLayer& el, const PanelLayout& panel, int clip) {
  for (std::size_t i = 0; i < el.x.size() && i < el.y.size() && i < el.sx.size() && i < el.sy.size() && i < el.rho.size();
       ++i) {
    if (!finite(el.x[i]) || !finite(el.y[i]) || !finite(el.sx[i]) || !finite(el.sy[i]) || !finite(el.rho[i])) continue;
    const bool ex = i < el.excluded.size() && el.excluded[i];
    const double rho = std::clamp(el.rho[i], -0.999999, 0.999999);
    const double q = std::sqrt(1 - rho * rho);
    const double ax = el.scale * el.sx[i], ay = el.scale * el.sy[i];
    bool ok = true;
    DrawPath p;
    p.role = DrawRole::Ellipse;
    p.clip = clip;
    p.ops = circle([&](double u, double v) {
      const double x = el.x[i] + ax * u, y = el.y[i] + ay * (rho * u + q * v);
      if (panel.log_y && y <= 0) ok = false;
      return Pt{map_x(panel, x), map_y(panel, y)};
    });
    for (const auto& op : p.ops)
      for (const auto& pt : op.p)
        if (!finite(pt.x) || !finite(pt.y)) ok = false;
    if (!ok) continue;
    p.stroke = ex ? kExcludedLine : el.line;
    p.dash = ex ? LineDash::Dash : LineDash::Solid;
    if (el.filled && !ex) p.fill = el.fill;
    b.path(std::move(p));
  }
}

void draw_guide(Builder& b, const GuideLayer& g, const PanelLayout& panel, int clip) {
  if (!finite(g.value) || (g.horizontal && panel.log_y && g.value <= 0)) return;
  DrawPath p;
  p.role = DrawRole::Guide;
  p.clip = clip;
  p.stroke = g.style.color;
  p.width = g.style.width;
  p.dash = g.style.dash;
  const Box& r = panel.plot;
  if (g.horizontal) {
    const double y = map_y(panel, g.value);
    if (!finite(y)) return;
    p.ops = {move(r.x, y), line(r.x + r.w, y)};
  } else {
    const double x = map_x(panel, g.value);
    if (!finite(x)) return;
    p.ops = {move(x, r.y), line(x, r.y + r.h)};
  }
  b.path(std::move(p));
}

void draw_span(Builder& b, const SpanLayer& s, const PanelLayout& panel, int clip) {
  const Box& r = panel.plot;
  const auto bound = [](const std::optional<double>& v) { return !v || finite(*v); };
  if (!bound(s.x0) || !bound(s.x1) || !bound(s.y0) || !bound(s.y1)) return;
  if (panel.log_y && ((s.y0 && *s.y0 <= 0) || (s.y1 && *s.y1 <= 0))) return;
  const double x0 = s.x0 ? map_x(panel, *s.x0) : r.x, x1 = s.x1 ? map_x(panel, *s.x1) : r.x + r.w;
  const double y1 = s.y1 ? map_y(panel, *s.y1) : r.y, y0 = s.y0 ? map_y(panel, *s.y0) : r.y + r.h;
  if (!finite(x0) || !finite(x1) || !finite(y0) || !finite(y1)) return;
  DrawPath p;
  p.role = DrawRole::Span;
  p.clip = clip;
  p.fill = s.fill;
  rect(p.ops, x0, y1, x1, y0);
  b.path(std::move(p));
  DrawText t;
  t.role = TextRole::SpanLabel;
  t.clip = clip;
  t.anchor = TextAnchor::Middle;
  t.size = b.style.fonts.annotation;
  t.at = {(x0 + x1) / 2, std::min(y0, y1) + 2 + t.size};
  t.text = s.label;
  b.text(std::move(t));
}

// A box of lines in a corner of the plot, at the view's corner ratios.
Box corner_box(const Box& plot, Corner corner, double w, double h) {
  const bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
  const bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
  const double ax = plot.x + plot.w * (right ? 0.99 : 0.01), ay = plot.y + plot.h * (bottom ? 0.98 : 0.02);
  return {right ? ax - w : ax, bottom ? ay - h : ay, w, h};
}

struct Line {
  std::string text;
  Color color;
};

void draw_corner_texts(Builder& b, const std::map<Corner, std::vector<Line>>& corners, const Box& plot, int clip) {
  const double size = b.style.fonts.annotation;
  for (const auto& [corner, lines] : corners) {
    double w = 0;
    for (const auto& l : lines) w = std::max(w, text_width(l.text, size));
    const Box box = corner_box(plot, corner, w + 8, static_cast<double>(lines.size()) * size * kLineHeight + 4);
    DrawPath p;
    p.role = DrawRole::TextBox;
    p.clip = clip;
    p.fill = kOverlay;
    rect(p.ops, box.x, box.y, box.x + box.w, box.y + box.h);
    b.path(std::move(p));
    for (std::size_t k = 0; k < lines.size(); ++k) {
      DrawText t;
      t.role = TextRole::Annotation;
      t.clip = clip;
      t.size = size;
      t.color = lines[k].color;
      t.at = {box.x + 4, box.y + 2 + size + static_cast<double>(k) * size * kLineHeight};
      t.text = lines[k].text;
      b.text(std::move(t));
    }
  }
}

void draw_legend(Builder& b, const Panel& panel, const Box& plot) {
  std::vector<const PointLayer*> items;
  for (const auto& layer : panel.layers)
    if (const auto* pts = std::get_if<PointLayer>(&layer); pts && !pts->label.empty()) {
      bool any = false;
      for (std::size_t i = 0; i < pts->x.size(); ++i)
        if (!(i < pts->excluded.size() && pts->excluded[i])) any = true;
      if (any) items.push_back(pts);
    }
  if (items.empty()) return;
  const double size = b.style.fonts.annotation, row = size * kLineHeight + 2;
  double w = 0;
  for (const auto* it : items) w = std::max(w, text_width(it->label, size));
  const Box box = corner_box(plot, b.style.legend_corner, w + row + 10, static_cast<double>(items.size()) * row + 4);
  DrawPath frame;
  frame.role = DrawRole::Legend;
  frame.fill = kOverlay;
  frame.stroke = kBlack;
  frame.width = 0.5;
  rect(frame.ops, box.x, box.y, box.x + box.w, box.y + box.h);
  b.path(std::move(frame));
  for (std::size_t k = 0; k < items.size(); ++k) {
    const double cy = box.y + 2 + row * (static_cast<double>(k) + 0.5);
    MarkerStyle m = items[k]->marker;
    m.size = std::min(m.size, size);
    b.path(marker_path(m, {box.x + 4 + row / 2, cy}, DrawRole::Legend, -1));
    DrawText t;
    t.role = TextRole::Legend;
    t.size = size;
    t.at = {box.x + 6 + row, cy + size * 0.35};
    t.text = items[k]->label;
    b.text(std::move(t));
  }
}

void draw_axes(Builder& b, const PanelLayout& pl, const Panel& panel, const Graph& g) {
  const Box& r = pl.plot;
  const Fonts& f = b.style.fonts;
  DrawPath axes;
  axes.role = DrawRole::Axis;
  axes.stroke = kBlack;
  axes.width = 1.0;
  axes.ops = {move(r.x, r.y), line(r.x, r.y + r.h), line(r.x + r.w, r.y + r.h)};
  const auto in_x = [&](double v) { return v >= r.x - 1e-9 && v <= r.x + r.w + 1e-9; };
  const auto in_y = [&](double v) { return v >= r.y - 1e-9 && v <= r.y + r.h + 1e-9; };
  for (const auto& t : pl.x_ticks) {
    const double x = map_x(pl, t.value);
    if (!finite(x) || !in_x(x)) continue;
    axes.ops.push_back(move(x, r.y + r.h));
    axes.ops.push_back(line(x, r.y + r.h - kTickLength));
  }
  for (const auto& t : pl.y_ticks) {
    const double y = map_y(pl, t.value);
    if (!finite(y) || !in_y(y)) continue;
    axes.ops.push_back(move(r.x, y));
    axes.ops.push_back(line(r.x + kTickLength, y));
  }
  b.path(std::move(axes));

  for (const auto& t : pl.y_ticks) {
    const double y = map_y(pl, t.value);
    if (!finite(y) || !in_y(y)) continue;
    DrawText txt;
    txt.role = TextRole::Tick;
    txt.size = f.tick;
    txt.anchor = TextAnchor::End;
    txt.at = {r.x - kTickGap, y + f.tick * 0.35};
    txt.text = t.label;
    b.text(std::move(txt));
  }
  double below = r.y + r.h + kTickGap;  // the baseline of the next line under the plot, less one size
  if (pl.x_tick_labels) {
    int lines = 1;
    for (const auto& t : pl.x_ticks) {
      const double x = map_x(pl, t.value);
      if (!finite(x) || !in_x(x)) continue;
      std::size_t start = 0;
      int k = 0;
      while (true) {
        const auto nl = t.label.find('\n', start);
        DrawText txt;
        txt.role = TextRole::Tick;
        txt.size = f.tick;
        txt.anchor = TextAnchor::Middle;
        txt.at = {x, below + f.tick + k * f.tick * kLineHeight};
        txt.text = t.label.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        b.text(std::move(txt));
        ++k;
        if (nl == std::string::npos) break;
        start = nl + 1;
      }
      lines = std::max(lines, k);
    }
    below += lines * f.tick * kLineHeight;
  }
  DrawText yt;
  yt.role = TextRole::AxisTitle;
  yt.size = f.axis_title;
  yt.anchor = TextAnchor::Middle;
  yt.vertical = true;
  yt.text = panel.y.title;
  // Glyphs of a rotated line lie on the left of its baseline.
  double widest = 0;
  for (const auto& t : pl.y_ticks) widest = std::max(widest, text_width(t.label, f.tick));
  yt.at = {r.x - kTickGap - widest - 4, r.y + r.h / 2};
  b.text(std::move(yt));
  if (pl.x_tick_labels) {
    DrawText xt;
    xt.role = TextRole::AxisTitle;
    xt.size = f.axis_title;
    xt.anchor = TextAnchor::Middle;
    xt.at = {r.x + r.w / 2, below + 2 + f.axis_title};
    xt.text = g.x.title;
    b.text(std::move(xt));
  }
}

void draw_grid(Builder& b, const PanelLayout& pl) {
  const Box& r = pl.plot;
  DrawPath grid;
  grid.role = DrawRole::Grid;
  grid.stroke = kGrid;
  grid.width = 0.5;
  for (const auto& t : pl.x_ticks) {
    const double x = map_x(pl, t.value);
    if (!finite(x) || x <= r.x + 1e-9 || x > r.x + r.w + 1e-9) continue;
    grid.ops.push_back(move(x, r.y));
    grid.ops.push_back(line(x, r.y + r.h));
  }
  for (const auto& t : pl.y_ticks) {
    const double y = map_y(pl, t.value);
    if (!finite(y) || y < r.y - 1e-9 || y >= r.y + r.h - 1e-9) continue;
    grid.ops.push_back(move(r.x, y));
    grid.ops.push_back(line(r.x + r.w, y));
  }
  b.path(std::move(grid));
}

}  // namespace

DrawList draw_scene(const Scene& scene, const FigureSize& size) {
  DrawList list;
  const SceneLayout layout = layout_scene(scene, size);
  list.width = layout.width;
  list.height = layout.height;
  list.background = scene.style.background;
  list.font_family = scene.style.fonts.family;
  Builder b{list, scene.style};
  if (scene.graphs.empty()) {
    DrawText t;
    t.role = TextRole::Empty;
    t.anchor = TextAnchor::Middle;
    t.size = scene.style.fonts.title;
    t.at = {list.width / 2, list.height / 2};
    t.text = "No data";
    b.text(std::move(t));
    return list;
  }
  for (std::size_t gi = 0; gi < scene.graphs.size(); ++gi) {
    const Graph& g = scene.graphs[gi];
    const GraphLayout& gl = layout.graphs[gi];
    for (std::size_t pi = 0; pi < g.panels.size(); ++pi) {
      const Panel& panel = g.panels[pi];
      const PanelLayout& pl = gl.panels[pi];
      const int clip = static_cast<int>(list.clips.size());
      list.clips.push_back(pl.plot);
      DrawPath bg;
      bg.role = DrawRole::Frame;
      bg.fill = scene.style.plot_background;
      rect(bg.ops, pl.plot.x, pl.plot.y, pl.plot.x + pl.plot.w, pl.plot.y + pl.plot.h);
      b.path(std::move(bg));
      if (scene.style.grid) draw_grid(b, pl);
      const Mapper map{pl};
      std::map<Corner, std::vector<Line>> corners;
      for (const auto& layer : panel.layers) {
        if (const auto* band = std::get_if<BandLayer>(&layer)) draw_band(b, *band, map, clip);
        else if (const auto* l = std::get_if<LineLayer>(&layer)) draw_line(b, *l, map, clip);
        else if (const auto* pts = std::get_if<PointLayer>(&layer)) draw_points(b, *pts, map, clip);
        else if (const auto* steps = std::get_if<StepLayer>(&layer)) draw_steps(b, *steps, map, clip);
        else if (const auto* el = std::get_if<EllipseLayer>(&layer)) draw_ellipses(b, *el, pl, clip);
        else if (const auto* guide = std::get_if<GuideLayer>(&layer)) draw_guide(b, *guide, pl, clip);
        else if (const auto* span = std::get_if<SpanLayer>(&layer)) draw_span(b, *span, pl, clip);
        else if (const auto* text = std::get_if<TextLayer>(&layer))
          for (const auto& t : text->lines) corners[text->corner].push_back({t, text->color});
      }
      draw_corner_texts(b, corners, pl.plot, clip);
      if (pi == 0 && scene.style.legend) draw_legend(b, panel, pl.plot);
      draw_axes(b, pl, panel, g);
    }
    DrawText title;
    title.role = TextRole::Title;
    title.bold = true;
    title.anchor = TextAnchor::Middle;
    title.size = scene.style.fonts.title;
    title.at = {gl.cell.x + gl.cell.w / 2, gl.cell.y + kOuter + title.size};
    title.text = gl.title;
    b.text(std::move(title));
  }
  return list;
}

std::string format_coord(double v) {
  if (!std::isfinite(v)) return "0";
  std::array<char, 400> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, 2);
  std::string s(buf.data(), r.ptr);
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  if (s == "-0") s = "0";
  return s;
}

std::vector<double> dash_pattern(LineDash dash) {
  switch (dash) {
    case LineDash::Dash:
      return {6, 3};
    case LineDash::Dot:
      return {1, 2};
    case LineDash::DashDot:
      return {6, 2, 1, 2};
    case LineDash::Solid:
      break;
  }
  return {};
}

}  // namespace pychron::processing
