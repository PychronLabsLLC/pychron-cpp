#include "pychron/experiment/model/positions.hpp"

#include <charconv>
#include <cstdlib>

namespace pychron::experiment {
namespace {

constexpr std::size_t kMaxHoles = 10000;

bool parse_int(std::string_view s, int& out) {
  if (s.empty()) return false;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size() && out >= 0;
}

bool is_sep(char c) { return c == ',' || c == ';' || c == ' ' || c == '\t'; }

}  // namespace

Result<Position> parse_position(std::string_view text) {
  Position pos;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && is_sep(text[i])) ++i;
    std::size_t j = i;
    while (j < text.size() && !is_sep(text[j])) ++j;
    if (j == i) break;
    std::string_view tok = text.substr(i, j - i);
    i = j;
    int a = 0, b = 0;
    if (auto dash = tok.find('-'); dash != std::string_view::npos) {
      if (!parse_int(tok.substr(0, dash), a) || !parse_int(tok.substr(dash + 1), b))
        return fail(ErrorKind::Config, "bad position range '" + std::string(tok) + "'");
      int step = a <= b ? 1 : -1;
      if (static_cast<std::size_t>(std::abs(b - a)) + 1 + pos.holes.size() > kMaxHoles)
        return fail(ErrorKind::Config, "position range '" + std::string(tok) + "' too large");
      for (int h = a;; h += step) {
        pos.holes.push_back(h);
        if (h == b) break;
      }
    } else {
      if (!parse_int(tok, a)) return fail(ErrorKind::Config, "bad position '" + std::string(tok) + "'");
      pos.holes.push_back(a);
    }
  }
  if (pos.holes.empty()) return fail(ErrorKind::Config, "empty position");
  return pos;
}

std::string format_position(const Position& p) {
  std::string out;
  const auto& h = p.holes;
  for (std::size_t i = 0; i < h.size();) {
    std::size_t j = i;
    while (j + 1 < h.size() && h[j + 1] == h[j] + 1) ++j;
    if (!out.empty()) out += ',';
    out += std::to_string(h[i]);
    if (j > i) out += (j == i + 1 ? "," : "-") + std::to_string(h[j]);
    i = j + 1;
  }
  return out;
}

std::vector<Position> split_position(const Position& p) {
  std::vector<Position> out;
  out.reserve(p.holes.size());
  for (int h : p.holes) out.push_back(Position{{h}});
  return out;
}

Result<Position> increment_position(const Position& p, int step) {
  Position out = p;
  for (int& h : out.holes) {
    h += step;
    if (h < 0) return fail(ErrorKind::Config, "position increment goes below zero");
  }
  return out;
}

}  // namespace pychron::experiment
