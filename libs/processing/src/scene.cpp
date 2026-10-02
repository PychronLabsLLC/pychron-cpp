#include "pychron/processing/scene.hpp"

#include <cctype>
#include <cstdio>
#include <utility>

namespace pychron::processing {

namespace {

constexpr std::pair<std::string_view, Color> kNamed[] = {
    {"black", {0, 0, 0, 255}},        {"white", {255, 255, 255, 255}}, {"red", {214, 39, 40, 255}},
    {"green", {44, 160, 44, 255}},    {"blue", {31, 119, 180, 255}},   {"orange", {255, 127, 14, 255}},
    {"purple", {148, 103, 189, 255}}, {"brown", {140, 86, 75, 255}},   {"gray", {127, 127, 127, 255}},
    {"grey", {127, 127, 127, 255}},   {"cyan", {23, 190, 207, 255}},   {"magenta", {227, 119, 194, 255}},
    {"yellow", {188, 189, 34, 255}},  {"navy", {0, 0, 128, 255}},      {"teal", {0, 128, 128, 255}},
    {"olive", {128, 128, 0, 255}},    {"maroon", {128, 0, 0, 255}},    {"pink", {247, 182, 210, 255}},
    {"lime", {50, 205, 50, 255}},     {"gold", {218, 165, 32, 255}},
};

// Tableau-like, readable on white, no near-white entries (legacy W12).
constexpr Color kPalette[] = {
    {31, 119, 180, 255}, {214, 39, 40, 255},  {44, 160, 44, 255},  {255, 127, 14, 255},
    {148, 103, 189, 255}, {140, 86, 75, 255}, {227, 119, 194, 255}, {127, 127, 127, 255},
    {188, 189, 34, 255}, {23, 190, 207, 255}, {0, 0, 128, 255},    {0, 0, 0, 255},
};

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

std::optional<Color> parse_color(std::string_view t) {
  if (!t.empty() && t[0] == '#' && (t.size() == 7 || t.size() == 9)) {
    std::uint8_t v[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i < (t.size() - 1) / 2; ++i) {
      const int hi = hex(t[1 + 2 * i]), lo = hex(t[2 + 2 * i]);
      if (hi < 0 || lo < 0) return std::nullopt;
      v[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return Color{v[0], v[1], v[2], v[3]};
  }
  for (const auto& [n, c] : kNamed)
    if (n == t) return c;
  return std::nullopt;
}

std::string color_hex(const Color& c) {
  char buf[16];
  if (c.a == 255)
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
  else
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
  return buf;
}

Color palette_color(int index) {
  constexpr int n = static_cast<int>(std::size(kPalette));
  return kPalette[((index % n) + n) % n];
}

std::string_view to_string(MarkerShape s) noexcept {
  switch (s) {
    case MarkerShape::Circle:
      return "circle";
    case MarkerShape::Square:
      return "square";
    case MarkerShape::Diamond:
      return "diamond";
    case MarkerShape::Triangle:
      return "triangle";
    case MarkerShape::Cross:
      return "cross";
    case MarkerShape::Plus:
      return "plus";
    case MarkerShape::Star:
      return "star";
  }
  return "circle";
}

std::optional<MarkerShape> parse_marker(std::string_view t) noexcept {
  for (auto s : {MarkerShape::Circle, MarkerShape::Square, MarkerShape::Diamond, MarkerShape::Triangle,
                 MarkerShape::Cross, MarkerShape::Plus, MarkerShape::Star})
    if (to_string(s) == t) return s;
  return std::nullopt;
}

std::string_view to_string(Corner c) noexcept {
  switch (c) {
    case Corner::TopLeft:
      return "top_left";
    case Corner::TopRight:
      return "top_right";
    case Corner::BottomLeft:
      return "bottom_left";
    case Corner::BottomRight:
      return "bottom_right";
  }
  return "top_left";
}

std::optional<Corner> parse_corner(std::string_view t) noexcept {
  for (auto c : {Corner::TopLeft, Corner::TopRight, Corner::BottomLeft, Corner::BottomRight})
    if (to_string(c) == t) return c;
  return std::nullopt;
}

std::size_t Scene::point_count() const {
  std::size_t n = 0;
  for (const auto& g : graphs)
    for (const auto& p : g.panels)
      for (const auto& l : p.layers)
        if (const auto* pts = std::get_if<PointLayer>(&l))
          n += pts->x.size();
        else if (const auto* steps = std::get_if<StepLayer>(&l))
          n += steps->x0.size();
  return n;
}

}  // namespace pychron::processing
