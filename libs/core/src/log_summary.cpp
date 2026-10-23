#include "pychron/core/log_summary.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace pychron {
namespace {

bool equals_nocase(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
  });
}

bool is_secret_key(std::string_view key) {
  static constexpr std::array<std::string_view, 4> names{"password", "token", "secret", "key"};
  for (const std::string_view name : names) {
    if (equals_nocase(key, name)) return true;
    if (key.size() > name.size()) {
      const std::string_view tail = key.substr(key.size() - name.size());
      const char sep = key[key.size() - name.size() - 1];
      if ((sep == '_' || sep == '-') && equals_nocase(tail, name)) return true;
    }
  }
  return false;
}

}  // namespace

std::string truncate_for_log(std::string_view text, std::size_t max) {
  if (text.size() <= max) return std::string{text};
  std::size_t cut = max;
  // Back up off a continuation byte (10xxxxxx) so the next byte starts a character.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
  std::string out{text.substr(0, cut)};
  out += "\xE2\x80\xA6";
  return out;
}

std::string summarize(std::span<const std::pair<std::string, std::string>> fields) {
  std::string out;
  for (const auto& [key, value] : fields) {
    if (!out.empty()) out += ", ";
    out += key;
    out += '=';
    out += is_secret_key(key) ? std::string_view{"***"} : std::string_view{value};
  }
  return truncate_for_log(out);
}

}  // namespace pychron
