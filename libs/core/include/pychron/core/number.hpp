#pragma once

// Text to number, the same on every platform. Not std::from_chars: its
// floating-point overloads are missing from Apple's libc++ (and from libc++
// before LLVM 20). Not strtod or a plain stream: they read the process
// locale's decimal point, and Qt sets that locale.

#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace pychron {

// The double the whole of `text` spells, in the C locale: an optional sign,
// digits, a decimal point, an exponent. Nothing else around it, not "inf" or
// "nan", not hex. nullopt otherwise.
inline std::optional<double> parse_double(std::string_view text) noexcept {
  if (text.empty() || text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')
    return std::nullopt;
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  double v = 0;
  char rest = 0;
  if (!(in >> v) || in.get(rest) || !std::isfinite(v)) return std::nullopt;
  return v;
}

// The longest prefix of `text` that spells a number (the characters a decimal
// number is made of: sign, digits, '.', 'e', 'E'), parsed with parse_double.
// Returns the value and how many characters it took; nullopt when the text
// does not begin with a number.
inline std::optional<std::pair<double, std::size_t>> parse_double_prefix(std::string_view text) noexcept {
  std::size_t n = 0;
  while (n < text.size()) {
    const char c = text[n];
    const bool numeric = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                         ((c == '+' || c == '-') && (n == 0 || text[n - 1] == 'e' || text[n - 1] == 'E'));
    if (!numeric) break;
    ++n;
  }
  // A trailing exponent marker (or sign) that never got its digits is not part of the number.
  while (n > 0) {
    if (const auto v = parse_double(text.substr(0, n))) return std::pair{*v, n};
    --n;
  }
  return std::nullopt;
}

}  // namespace pychron
