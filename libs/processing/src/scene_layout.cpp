// The layout of a scene (data publishing design, section 5.6).

#include "pychron/processing/scene_layout.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace pychron::processing {

namespace {

// Advance widths of printable ASCII (32..126) in 1/1000 em, from the AFM
// files of the standard 14 fonts.
constexpr std::array<std::uint16_t, 95> kHelvetica = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,   // ' ' .. '/'
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,                                 // '0' .. '9'
    278, 278, 584, 584, 584, 556, 1015,                                               // ':' .. '@'
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833,                  // 'A' .. 'M'
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,                  // 'N' .. 'Z'
    278, 278, 278, 469, 556, 333,                                                     // '[' .. '`'
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833,                  // 'a' .. 'm'
    556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500,                  // 'n' .. 'z'
    334, 260, 334, 584,                                                               // '{' .. '~'
};
constexpr std::array<std::uint16_t, 95> kHelveticaBold = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,
    333, 333, 584, 584, 584, 611, 975,
    722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833,
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,
    333, 278, 333, 584, 556, 333,
    556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889,
    611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500,
    389, 280, 389, 584,
};

int glyph_width(char32_t c, bool bold) {
  if (c >= 32 && c <= 126) return (bold ? kHelveticaBold : kHelvetica)[c - 32];
  switch (c) {
    case U'±':  // plus-minus
      return 584;
    case U'°':  // degree
      return 400;
    case U'µ':  // micro
    case U'μ':
      return 556;
    case U'σ':  // sigma (Symbol)
      return 603;
    default:
      return 556;
  }
}

// Next code point of UTF-8 text. A byte that does not start a well-formed
// sequence (a lone continuation, an overlong form, a surrogate, a value above
// U+10FFFF, a truncated sequence) is U+FFFD and one byte.
char32_t decode_utf8(std::string_view s, std::size_t& i) {
  const auto b = static_cast<unsigned char>(s[i]);
  if (b < 0x80) {
    ++i;
    return b;
  }
  int n = 0;
  char32_t cp = 0, least = 0;
  if (b >= 0xC2 && b <= 0xDF) {
    n = 1;
    cp = b & 0x1FU;
    least = 0x80;
  } else if (b >= 0xE0 && b <= 0xEF) {
    n = 2;
    cp = b & 0x0FU;
    least = 0x800;
  } else if (b >= 0xF0 && b <= 0xF4) {
    n = 3;
    cp = b & 0x07U;
    least = 0x10000;
  } else {
    ++i;
    return 0xFFFD;
  }
  for (int k = 1; k <= n; ++k) {
    const std::size_t at = i + static_cast<std::size_t>(k);
    if (at >= s.size() || (static_cast<unsigned char>(s[at]) & 0xC0U) != 0x80U) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (static_cast<unsigned char>(s[at]) & 0x3FU);
  }
  if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++i;
    return 0xFFFD;
  }
  i += static_cast<std::size_t>(n) + 1;
  return cp;
}

// The extent of a set of values, the finite ones only.
struct Extent {
  double lo = 0, hi = 0;
  bool found = false;
  bool positive_only = false;  // a log axis: values <= 0 are not data
  void add(double v) {
    if (!std::isfinite(v) || (positive_only && v <= 0)) return;
    if (!found) {
      lo = hi = v;
      found = true;
      return;
    }
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
};

bool shown(const PointLayer& p, std::size_t i) {
  const bool ex = i < p.excluded.size() && p.excluded[i];
  return !ex || p.show_excluded;
}

void add_x(Extent& e, const Layer& layer) {
  if (const auto* pts = std::get_if<PointLayer>(&layer)) {
    for (std::size_t i = 0; i < pts->x.size(); ++i) {
      if (!shown(*pts, i)) continue;
      const double err = i < pts->x_err.size() ? pts->x_err[i] : 0.0;
      e.add(pts->x[i]);
      e.add(pts->x[i] - err);
      e.add(pts->x[i] + err);
    }
  } else if (const auto* line = std::get_if<LineLayer>(&layer)) {
    for (const double v : line->x) e.add(v);
  } else if (const auto* band = std::get_if<BandLayer>(&layer)) {
    for (const double v : band->x) e.add(v);
  } else if (const auto* steps = std::get_if<StepLayer>(&layer)) {
    for (const double v : steps->x0) e.add(v);
    for (const double v : steps->x1) e.add(v);
  } else if (const auto* el = std::get_if<EllipseLayer>(&layer)) {
    for (std::size_t i = 0; i < el->x.size() && i < el->sx.size(); ++i) {
      e.add(el->x[i] - el->scale * el->sx[i]);
      e.add(el->x[i] + el->scale * el->sx[i]);
    }
  }
}

void add_y(Extent& e, const Layer& layer) {
  if (const auto* pts = std::get_if<PointLayer>(&layer)) {
    for (std::size_t i = 0; i < pts->y.size(); ++i) {
      if (!shown(*pts, i)) continue;
      const double err = i < pts->y_err.size() ? pts->y_err[i] : 0.0;
      e.add(pts->y[i]);
      e.add(pts->y[i] - err);
      e.add(pts->y[i] + err);
    }
  } else if (const auto* line = std::get_if<LineLayer>(&layer)) {
    for (const double v : line->y) e.add(v);
  } else if (const auto* band = std::get_if<BandLayer>(&layer)) {
    for (const double v : band->low) e.add(v);
    for (const double v : band->high) e.add(v);
  } else if (const auto* steps = std::get_if<StepLayer>(&layer)) {
    for (std::size_t i = 0; i < steps->y.size(); ++i) {
      const double err = i < steps->y_err.size() ? steps->y_err[i] : 0.0;
      e.add(steps->y[i]);
      e.add(steps->y[i] - err);
      e.add(steps->y[i] + err);
    }
  } else if (const auto* el = std::get_if<EllipseLayer>(&layer)) {
    for (std::size_t i = 0; i < el->y.size() && i < el->sy.size(); ++i) {
      e.add(el->y[i] - el->scale * el->sy[i]);
      e.add(el->y[i] + el->scale * el->sy[i]);
    }
  }
}

Range padded(const Extent& e) {
  double pad = (e.hi - e.lo) * 0.08;
  if (pad == 0) pad = std::max(std::abs(e.hi) * 0.05, 1e-12);
  return {e.lo - pad, e.hi + pad};
}

bool finite_increasing(Range r) { return std::isfinite(r.min) && std::isfinite(r.max) && r.max > r.min; }

std::size_t tick_limit(int target) { return static_cast<std::size_t>(4 * std::max(1, target) + 2); }

// "1.2300" -> "1.23", "2." -> "2", "-0" -> "0".
std::string strip_zeros(std::string s) {
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

std::string fixed(double v, int decimals) {
  std::array<char, 400> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, decimals);
  return strip_zeros(std::string(buf.data(), r.ptr));
}

// "2.5e6", "1e-5": the mantissa with `precision` decimals, trailing zeros dropped.
std::string scientific(double v, int precision) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::scientific, precision);
  std::string s(buf.data(), r.ptr);
  const auto e = s.find('e');
  if (e == std::string::npos) return s;
  int exp = 0;
  std::from_chars(s.data() + e + 1 + (s[e + 1] == '+' ? 1 : 0), s.data() + s.size(), exp);
  return strip_zeros(s.substr(0, e)) + "e" + std::to_string(exp);
}

// 10^k by IEEE multiplication and division alone, so the same double under
// every compiler (no libm, and no floating-point from_chars, which Apple's
// libc++ lacks). Exact, or correctly rounded, for |k| <= 22; beyond, the same
// rounding steps everywhere. 0 or inf past the double range.
double pow10(int k) {
  constexpr int kExact = 22;  // 10^22 is the largest power of ten a double holds exactly
  double exact = 1.0;
  for (int i = 0; i < std::min(std::abs(k), kExact); ++i) exact *= 10.0;
  if (k >= 0) {
    for (int i = kExact; i < k && std::isfinite(exact); ++i) exact *= 10.0;
    return exact;
  }
  double v = 1.0 / exact;
  for (int i = kExact; i < -k && v > 0; ++i) v /= 10.0;
  return v;
}

// The decade of x > 0: the k with 10^k <= x < 10^(k+1). log10 gives a first
// guess that a libm may round across a power of ten; the comparisons with
// pow10 settle it the same way everywhere.
int decade(double x) {
  int k = static_cast<int>(std::floor(std::log10(x)));
  if (pow10(k) > x) --k;
  else if (pow10(k + 1) <= x) ++k;
  return k;
}

std::vector<Tick> linear_ticks(Range r, int target) {
  if (!finite_increasing(r)) return {};
  const double raw = (r.max - r.min) / std::max(1, target);
  if (!std::isfinite(raw) || raw <= 0) return {};
  int e = decade(raw);
  const double f = raw / pow10(e);
  int nice = f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10;
  if (nice == 10) {
    nice = 1;
    ++e;
  }
  const double step = nice * pow10(e);
  if (!std::isfinite(step) || step <= 0) return {};
  const double k0 = std::ceil(r.min / step - 1e-9), k1 = std::floor(r.max / step + 1e-9);
  if (!(k1 >= k0) || k1 - k0 + 1 > static_cast<double>(tick_limit(target))) return {};
  const bool sci = std::max(std::abs(r.min), std::abs(r.max)) >= 1e6 || step < 1e-5;
  std::vector<Tick> out;
  const auto count = static_cast<int>(k1 - k0) + 1;
  for (int i = 0; i < count; ++i) {
    double v = (k0 + i) * step;
    if (std::abs(v) < step * 1e-6) v = 0;
    std::string label;
    if (v == 0)
      label = "0";
    else if (!sci)
      label = fixed(v, std::max(0, -e));
    else
      label = scientific(v, std::max(0, decade(std::abs(v)) - e));
    out.push_back({v, std::move(label)});
  }
  return out;
}

std::vector<Tick> log_ticks(Range r, int target) {
  if (!finite_increasing(r) || r.min <= 0) return {};
  const int lo = decade(r.min) + (pow10(decade(r.min)) < r.min ? 1 : 0);
  const int hi = decade(r.max);
  if (hi < lo) return {};
  const int n = hi - lo + 1;
  const int per = 2 * std::max(1, target);
  const int stride = std::max(1, (n + per - 1) / per);
  std::vector<Tick> out;
  for (int k = lo; k <= hi; k += stride) {
    const double v = pow10(k);
    out.push_back({v, k >= -3 && k <= 4 ? fixed(v, std::max(0, -k)) : "1e" + std::to_string(k)});
  }
  return out;
}

// Days since 1970-01-01 -> civil date (H. Hinnant's algorithm).
void civil_from_days(std::int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

std::string two(int v) { return (v < 10 ? "0" : "") + std::to_string(v); }

// A year as four digits (0001, 2025), as a PDF date and ISO 8601 want it.
std::string four(int v) {
  std::string s = std::to_string(v);
  if (v >= 0 && s.size() < 4) s.insert(0, 4 - s.size(), '0');
  return s;
}

// Times beyond this many seconds from 1970 (about 317,000 years) are not
// written: their days and years would not fit the arithmetic below.
constexpr double kMaxSeconds = 1e13;

std::string utc_text(double seconds, const std::string& format) {
  if (!(std::abs(seconds) <= kMaxSeconds)) return {};
  const auto t = static_cast<std::int64_t>(std::floor(seconds));
  std::int64_t days = t / 86400, rem = t % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  int y = 0, m = 0, d = 0;
  civil_from_days(days, y, m, d);
  const int hh = static_cast<int>(rem / 3600), mm = static_cast<int>(rem % 3600 / 60), ss = static_cast<int>(rem % 60);
  static constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::string f = format.empty() ? "%Y-%m-%d%n%H:%M" : format;
  std::string out;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f[i] != '%' || i + 1 >= f.size()) {
      out.push_back(f[i]);
      continue;
    }
    switch (f[++i]) {
      case 'Y': out += four(y); break;
      case 'y': out += two(((y % 100) + 100) % 100); break;
      case 'm': out += two(m); break;
      case 'd': out += two(d); break;
      case 'b': out += kMonths[static_cast<std::size_t>(m - 1)]; break;
      case 'H': out += two(hh); break;
      case 'M': out += two(mm); break;
      case 'S': out += two(ss); break;
      case 'n': out += '\n'; break;
      case '%': out += '%'; break;
      default: break;
    }
  }
  return out;
}

std::vector<Tick> time_ticks(const Axis& axis, Range r, int target) {
  if (!finite_increasing(r) || std::max(std::abs(r.min), std::abs(r.max)) > kMaxSeconds) return {};
  static constexpr std::array<double, 24> kSteps = {
      1,     2,     5,     10,    15,     30,     60,     120,     300,     600,     900,     1800,
      3600,  7200,  10800, 21600, 43200,  86400,  172800, 604800,  1209600, 2592000, 7776000, 31536000};
  const double raw = (r.max - r.min) / std::max(1, target);
  double step = 0;
  for (const double s : kSteps)
    if (s >= raw) {
      step = s;
      break;
    }
  if (step == 0) step = kSteps.back() * std::ceil(raw / kSteps.back());
  if (!std::isfinite(step)) return {};
  const double k0 = std::ceil(r.min / step), k1 = std::floor(r.max / step);
  if (!(k1 >= k0) || k1 - k0 + 1 > static_cast<double>(tick_limit(target))) return {};
  std::vector<Tick> out;
  const auto count = static_cast<int>(k1 - k0) + 1;
  for (int i = 0; i < count; ++i) {
    const double v = (k0 + i) * step;
    out.push_back({v, utc_text(v, axis.time_format)});
  }
  return out;
}

std::vector<Tick> category_ticks(const Axis& axis, Range r, int target) {
  if (!finite_increasing(r)) return {};
  const double k0 = std::ceil(r.min), k1 = std::floor(r.max);
  if (!(k1 >= k0)) return {};
  const auto limit = static_cast<double>(tick_limit(target));
  const double stride = std::max(1.0, std::ceil((k1 - k0 + 1) / limit));
  const auto count = static_cast<int>(std::floor((k1 - k0) / stride)) + 1;
  std::vector<Tick> out;
  for (int i = 0; i < count; ++i) {
    const double k = k0 + i * stride;
    std::string label;
    if (k >= 0 && k < static_cast<double>(axis.categories.size())) label = axis.categories[static_cast<std::size_t>(k)];
    out.push_back({k, std::move(label)});
  }
  return out;
}

double widest_label(const std::vector<Tick>& ticks, double size) {
  double w = 0;
  for (const auto& t : ticks) {
    std::size_t start = 0;
    while (start <= t.label.size()) {
      const auto nl = t.label.find('\n', start);
      const auto end = nl == std::string::npos ? t.label.size() : nl;
      w = std::max(w, text_width(std::string_view(t.label).substr(start, end - start), size));
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }
  return w;
}

constexpr double kLineHeight = 1.2;   // of a font size
constexpr double kTitleHeight = 1.4;  // a title line and the space under it
constexpr double kTickGap = 5.0;      // tick label to its axis
constexpr double kOuter = 8.0;        // page edge to the outermost text
constexpr double kRightMargin = 12.0;

}  // namespace

Range graph_x_range(const Graph& graph) {
  if (graph.x.min && graph.x.max) return {*graph.x.min, *graph.x.max};
  Extent e;
  for (const auto& p : graph.panels)
    for (const auto& layer : p.layers) add_x(e, layer);
  if (!e.found) return {};
  if (e.hi == e.lo) {
    const double pad = std::max(std::abs(e.hi) * 0.05, 1e-12);
    return {e.lo - pad, e.hi + pad};
  }
  return {e.lo, e.hi};
}

Range panel_y_range(const Panel& panel) {
  const bool log = panel.y.scale == AxisScale::Log;
  Extent e;
  e.positive_only = log;
  for (const auto& layer : panel.layers) add_y(e, layer);
  Range r;
  if (!e.found) {
    r = log ? Range{0.1, 10.0} : Range{0.0, 1.0};
  } else if (log) {
    const double lo = std::max(e.lo, e.hi * 1e-6);
    r = {lo / 1.2, e.hi * 1.2};
  } else {
    r = padded(e);
  }
  if (panel.y.min) r.min = *panel.y.min;
  if (panel.y.max) r.max = *panel.y.max;
  return r;
}

std::vector<Tick> axis_ticks(const Axis& axis, Range r, int target) {
  switch (axis.format) {
    case AxisFormat::Time:
      return time_ticks(axis, r, target);
    case AxisFormat::Category:
      return category_ticks(axis, r, target);
    case AxisFormat::Number:
      break;
  }
  return axis.scale == AxisScale::Log ? log_ticks(r, target) : linear_ticks(r, target);
}

char32_t next_code_point(std::string_view utf8, std::size_t& i) { return decode_utf8(utf8, i); }

std::string format_utc(double seconds, const std::string& format) { return utc_text(seconds, format); }

double text_width(std::string_view utf8, double size, bool bold) {
  long total = 0;
  for (std::size_t i = 0; i < utf8.size();) total += glyph_width(decode_utf8(utf8, i), bold);
  return static_cast<double>(total) * size / 1000.0;
}

SceneLayout layout_scene(const Scene& scene, const FigureSize& size) {
  SceneLayout out;
  const auto n = static_cast<int>(scene.graphs.size());
  const int columns = std::clamp(scene.columns, 1, std::max(1, n));
  const int used = std::max(1, std::min(columns, n));
  const int rows = std::max(1, (n + columns - 1) / columns);
  const double gap = scene.style.graph_spacing;
  out.width = used * size.graph_width + (used - 1) * gap;
  out.height = rows * size.graph_height + (rows - 1) * gap;
  const Fonts& fonts = scene.style.fonts;

  for (int gi = 0; gi < n; ++gi) {
    const Graph& g = scene.graphs[static_cast<std::size_t>(gi)];
    GraphLayout gl;
    gl.title = g.title;
    const int row = gi / columns;
    gl.cell = {(gi % columns) * (size.graph_width + gap), (size.graph_height + gap) * row, size.graph_width,
               size.graph_height};
    if (g.panels.empty()) {
      out.graphs.push_back(std::move(gl));
      continue;
    }
    const Range x = graph_x_range(g);
    const double top = g.title.empty() ? kOuter + 2 : kOuter + fonts.title * kTitleHeight;
    // The x tick labels' lines: a time format may break them in two.
    int x_lines = 1;
    if (g.x.format == AxisFormat::Time) {
      const std::string f = g.x.time_format.empty() ? "%Y-%m-%d%n%H:%M" : g.x.time_format;
      std::size_t pos = 0;
      while ((pos = f.find("%n", pos)) != std::string::npos) {
        ++x_lines;
        pos += 2;
      }
    }
    const double bottom = kTickGap + x_lines * fonts.tick * kLineHeight +
                          (g.x.title.empty() ? 0.0 : fonts.axis_title * kTitleHeight) + kOuter - 2;
    double weights = 0;
    for (const auto& p : g.panels) weights += std::max(0.05, p.height);
    const double spacing = scene.style.panel_spacing;
    const double avail = std::max(1.0, gl.cell.h - top - bottom - spacing * static_cast<double>(g.panels.size() - 1));

    bool any_y_title = false;
    double y_label_w = 0;
    double y = gl.cell.y + top;
    for (std::size_t pi = 0; pi < g.panels.size(); ++pi) {
      const Panel& p = g.panels[pi];
      PanelLayout pl;
      pl.id = p.id;
      pl.x = x;
      pl.y = panel_y_range(p);
      pl.log_y = p.y.scale == AxisScale::Log;
      if (pl.log_y && pl.y.min <= 0) pl.y.min = pl.y.max * 1e-6;
      pl.plot.y = y;
      pl.plot.h = avail * std::max(0.05, p.height) / weights;
      y += pl.plot.h + spacing;
      pl.y_ticks = axis_ticks(p.y, pl.y, std::clamp(static_cast<int>(pl.plot.h / 45.0), 2, 6));
      pl.x_tick_labels = pi + 1 == g.panels.size();
      y_label_w = std::max(y_label_w, widest_label(pl.y_ticks, fonts.tick));
      if (!p.y.title.empty()) any_y_title = true;
      gl.panels.push_back(std::move(pl));
    }
    const double left = kOuter + (any_y_title ? fonts.axis_title * kTitleHeight : 0.0) + y_label_w + kTickGap;
    const double width = std::max(1.0, gl.cell.w - left - kRightMargin);
    const auto x_ticks = axis_ticks(g.x, x, std::clamp(static_cast<int>(width / 90.0), 2, 8));
    for (auto& pl : gl.panels) {
      pl.plot.x = gl.cell.x + left;
      pl.plot.w = width;
      pl.x_ticks = x_ticks;
    }
    out.graphs.push_back(std::move(gl));
  }
  return out;
}

double map_x(const PanelLayout& p, double x) {
  const double span = p.x.max - p.x.min;
  if (span == 0) return p.plot.x + p.plot.w / 2;
  return p.plot.x + (x - p.x.min) / span * p.plot.w;
}

double map_y(const PanelLayout& p, double y) {
  double lo = p.y.min, hi = p.y.max, v = y;
  if (p.log_y) {
    lo = std::log10(lo);
    hi = std::log10(hi);
    v = std::log10(y);
  }
  const double span = hi - lo;
  if (span == 0) return p.plot.y + p.plot.h / 2;
  return p.plot.y + p.plot.h - (v - lo) / span * p.plot.h;
}

}  // namespace pychron::processing
