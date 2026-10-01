#pragma once

#include <string_view>

namespace pychron {

// Logger-name rule matching. A pattern without '*' is a bare name: it matches
// the logger of the same name and any dotted child ("a.b" matches "a.b.c").
// A pattern containing '*' is a glob where '*' matches any sequence of
// characters (including dots) over the whole logger name. An empty pattern
// matches nothing.
bool log_name_matches(std::string_view pattern, std::string_view logger) noexcept;

// Higher is more specific: (literal_chars << 1) | (no_star ? 1 : 0).
int log_rule_specificity(std::string_view pattern) noexcept;

}  // namespace pychron
