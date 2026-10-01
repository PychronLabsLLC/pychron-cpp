#include "pychron/core/log_match.hpp"

#include <cstddef>

namespace pychron {

namespace {

bool glob_match(std::string_view p, std::string_view s) noexcept {
  std::size_t pi = 0, si = 0;
  std::size_t star = std::string_view::npos, mark = 0;
  while (si < s.size()) {
    if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = si;
    } else if (pi < p.size() && p[pi] == s[si]) {
      ++pi;
      ++si;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

}  // namespace

bool log_name_matches(std::string_view pattern, std::string_view logger) noexcept {
  if (pattern.empty()) return false;
  if (pattern.find('*') != std::string_view::npos) return glob_match(pattern, logger);
  if (logger == pattern) return true;
  return logger.size() > pattern.size() && logger.compare(0, pattern.size(), pattern) == 0 &&
         logger[pattern.size()] == '.';
}

int log_rule_specificity(std::string_view pattern) noexcept {
  int literals = 0;
  bool has_star = false;
  for (char c : pattern) {
    if (c == '*') has_star = true;
    else ++literals;
  }
  return (literals << 1) | (has_star ? 0 : 1);
}

}  // namespace pychron
