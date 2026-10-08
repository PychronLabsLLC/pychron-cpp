#pragma once

// What a term is compared by, in one place: the session, the lookup, the
// pre-flight check and the vocabulary all trim a name and compare it without
// case, and must mean the same by both. Internal to pychron::earthbank.

#include <cstddef>
#include <string_view>

namespace pychron::earthbank::detail {

// A space, a tab or a line end: what stands around a name and is no part of
// it.
constexpr bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// `text` less the blanks around it.
constexpr std::string_view trimmed(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

constexpr char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The same text but for the case of ASCII letters.
constexpr bool same_but_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

}  // namespace pychron::earthbank::detail
