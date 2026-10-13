#include "pychron/entry/names.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace pychron::entry {
namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "<digits>" + 1 in decimal, kept at least as wide as the input.
std::string increment_digits(const std::string& digits) {
  std::string out = digits;
  int i = static_cast<int>(out.size()) - 1;
  for (; i >= 0; --i) {
    if (out[static_cast<std::size_t>(i)] == '9') {
      out[static_cast<std::size_t>(i)] = '0';
    } else {
      ++out[static_cast<std::size_t>(i)];
      break;
    }
  }
  if (i < 0) out.insert(out.begin(), '1');
  return out;
}

}  // namespace

std::string trim(std::string_view text) {
  std::size_t b = 0, e = text.size();
  while (b < e && is_space(text[b])) ++b;
  while (e > b && is_space(text[e - 1])) --e;
  return std::string(text.substr(b, e - b));
}

Result<PiName> parse_pi(std::string_view text, const std::vector<std::string>& allowed) {
  const std::string t = trim(text);
  if (std::find(allowed.begin(), allowed.end(), t) != allowed.end()) return PiName{t, ""};
  static const std::regex pattern(R"(^([A-Z][A-Za-z'\-]+)(?:, ?([A-Z]))?$)");
  std::smatch m;
  if (!std::regex_match(t, m, pattern))
    return fail(ErrorKind::Config, "principal investigator '" + t + R"(': write "Last" or "Last, F")");
  return PiName{m[1].str(), m[2].matched ? m[2].str() : std::string()};
}

std::string display_name(const PiName& name) {
  return name.first_initial.empty() ? name.last_name : name.last_name + ", " + name.first_initial;
}

bool valid_project_name(std::string_view name) {
  static const std::regex pattern(R"(^[A-Za-z][-A-Za-z0-9_]*$)");
  const std::string t(name);
  return std::regex_match(t, pattern);
}

bool valid_package_name(std::string_view name) {
  return !name.empty() && std::none_of(name.begin(), name.end(), is_space);
}

std::string next_package_name(const std::vector<std::string>& existing, std::string_view prefix) {
  std::string best;  // digits of the largest number, as written
  for (const auto& name : existing) {
    if (name.size() <= prefix.size() || !name.starts_with(prefix)) continue;
    const std::string digits = name.substr(prefix.size());
    if (!std::all_of(digits.begin(), digits.end(), is_digit)) continue;
    const auto value = [](const std::string& d) {
      const auto first = d.find_first_not_of('0');
      return first == std::string::npos ? std::string() : d.substr(first);
    };
    const std::string a = value(digits), b = value(best);
    if (best.empty() || a.size() > b.size() || (a.size() == b.size() && a > b) ||
        (a == b && digits.size() > best.size()))
      best = digits;
  }
  if (best.empty()) return std::string(prefix) + "001";
  std::string next = increment_digits(best);
  if (next.size() < 3) next.insert(0, 3 - next.size(), '0');
  return std::string(prefix) + next;
}

std::string next_letters(std::string_view letters) {
  if (letters.empty() || !std::all_of(letters.begin(), letters.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
    return "A";
  std::string out(letters);
  int i = static_cast<int>(out.size()) - 1;
  for (; i >= 0; --i) {
    if (out[static_cast<std::size_t>(i)] == 'Z') {
      out[static_cast<std::size_t>(i)] = 'A';
    } else {
      ++out[static_cast<std::size_t>(i)];
      break;
    }
  }
  if (i < 0) out.insert(out.begin(), 'A');
  return out;
}

std::string next_level_name(const std::vector<std::string>& existing) {
  std::string best;
  for (const auto& name : existing) {
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) continue;
    if (best.empty() || name.size() > best.size() || (name.size() == best.size() && name > best)) best = name;
  }
  return best.empty() ? "A" : next_letters(best);
}

bool valid_packet(std::string_view packet) {
  std::size_t i = 0;
  while (i < packet.size() && std::isalpha(static_cast<unsigned char>(packet[i]))) ++i;
  if (i == packet.size()) return false;
  return std::all_of(packet.begin() + static_cast<std::ptrdiff_t>(i), packet.end(), is_digit);
}

std::optional<std::string> next_packet(std::string_view packet) {
  if (!valid_packet(packet)) return std::nullopt;
  std::size_t i = 0;
  while (std::isalpha(static_cast<unsigned char>(packet[i]))) ++i;
  return std::string(packet.substr(0, i)) + increment_digits(std::string(packet.substr(i)));
}

}  // namespace pychron::entry
