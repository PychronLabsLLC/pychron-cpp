#include "legacy_json.hpp"

#include <cmath>
#include <limits>
#include <locale>
#include <sstream>

namespace pychron::dvc {

namespace {

// A token is written into the JSON text as a string starting with U+0001,
// which Python never emits unescaped and no legacy value contains; after
// parsing, those strings are replaced by null and reported.
constexpr char kTag = '\x01';
constexpr std::string_view kTokens[] = {"-Infinity", "Infinity", "NaN"};

// Rewrites bare NaN / Infinity / -Infinity outside strings into tagged
// strings. Returns how many were rewritten.
std::size_t tag_nonfinite(std::string_view text, std::string& out) {
  std::size_t count = 0;
  out.reserve(text.size());
  bool in_string = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      out.push_back(c);
      if (c == '\\' && i + 1 < text.size()) {
        out.push_back(text[++i]);
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      out.push_back(c);
      continue;
    }
    bool replaced = false;
    if (c == 'N' || c == 'I' || c == '-') {
      for (const auto token : kTokens) {
        if (text.substr(i, token.size()) != token) continue;
        // "NaNx" becomes a string followed by x, which the parser rejects.
        out += "\"\\u0001";
        out += token;
        out.push_back('"');
        i += token.size() - 1;
        ++count;
        replaced = true;
        break;
      }
    }
    if (!replaced) out.push_back(c);
  }
  return count;
}

bool is_tagged(const Json& j, std::string_view& token) {
  if (!j.is_string()) return false;
  const auto& s = j.get_ref<const std::string&>();
  if (s.empty() || s.front() != kTag) return false;
  for (const auto t : kTokens)
    if (std::string_view(s).substr(1) == t) {
      token = t;
      return true;
    }
  return false;
}

void untag(Json& j, const Json::json_pointer& at, std::vector<NonFinite>* seen) {
  std::string_view token;
  if (is_tagged(j, token)) {
    if (seen) seen->push_back(NonFinite{at.to_string(), std::string(token)});
    j = nullptr;
  } else if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) untag(it.value(), at / it.key(), seen);
  } else if (j.is_array()) {
    for (std::size_t i = 0; i < j.size(); ++i) untag(j[i], at / i, seen);
  }
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

bool is_empty_string(const Json& j) { return j.is_string() && j.get_ref<const std::string&>().empty(); }

template <class T, class Convert>
std::optional<T> take(Json& object, std::string_view key, Convert convert) {
  if (!object.is_object()) return std::nullopt;
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  if (it->is_null() || is_empty_string(*it)) {
    object.erase(it);
    return std::nullopt;
  }
  std::optional<T> value = convert(*it);
  if (value) object.erase(it);
  return value;
}

}  // namespace

Result<Json> parse_legacy(std::string_view text, std::vector<NonFinite>* nonfinite) {
  if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  std::string tagged;
  const std::size_t count = tag_nonfinite(text, tagged);
  try {
    Json j = Json::parse(tagged);
    if (count > 0) untag(j, Json::json_pointer(), nonfinite);
    return j;
  } catch (const Json::exception& e) {
    return fail(ErrorKind::Protocol, std::string("not JSON: ") + e.what());
  }
}

std::string dump(const Json& j) {
  // error_handler replace: invalid UTF-8 in a legacy string must not throw.
  return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<double> as_double(const Json& j) {
  if (j.is_number()) {
    const double v = j.get<double>();
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
  }
  if (j.is_string()) {
    const std::string_view s = trim(j.get_ref<const std::string&>());
    if (s.empty()) return std::nullopt;
    // from_chars accepts "nan" and "inf"; a field that says so is not a number.
    const char first = s.front() == '-' || s.front() == '+' ? (s.size() > 1 ? s[1] : '\0') : s.front();
    if (!((first >= '0' && first <= '9') || first == '.')) return std::nullopt;
    const std::string_view digits = s.front() == '+' ? s.substr(1) : s;
    // A stream in the C locale: floating-point from_chars is missing from
    // Apple's libc++, and strtod reads the process locale's decimal point.
    std::istringstream in{std::string(digits)};
    in.imbue(std::locale::classic());
    double v = 0;
    char rest = 0;
    if (!(in >> v) || in.get(rest) || !std::isfinite(v)) return std::nullopt;
    return v;
  }
  return std::nullopt;
}

std::optional<int> as_int(const Json& j) {
  if (j.is_boolean()) return std::nullopt;
  const auto v = as_double(j);
  if (!v || std::floor(*v) != *v) return std::nullopt;
  if (*v < static_cast<double>(std::numeric_limits<int>::min()) ||
      *v > static_cast<double>(std::numeric_limits<int>::max()))
    return std::nullopt;
  return static_cast<int>(*v);
}

std::optional<bool> as_bool(const Json& j) {
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number()) {
    const double v = j.get<double>();
    if (v == 0) return false;
    if (v == 1) return true;
    return std::nullopt;
  }
  if (j.is_string()) {
    std::string s(trim(j.get_ref<const std::string&>()));
    for (auto& c : s)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (s == "true") return true;
    if (s == "false") return false;
  }
  return std::nullopt;
}

std::optional<std::string> as_text(const Json& j) {
  if (j.is_string()) return j.get<std::string>();
  if (j.is_number() || j.is_boolean()) return dump(j);
  return std::nullopt;
}

std::optional<double> take_double(Json& object, std::string_view key) { return take<double>(object, key, as_double); }
std::optional<int> take_int(Json& object, std::string_view key) { return take<int>(object, key, as_int); }
std::optional<bool> take_bool(Json& object, std::string_view key) { return take<bool>(object, key, as_bool); }
std::optional<std::string> take_text(Json& object, std::string_view key) {
  return take<std::string>(object, key, as_text);
}

bool is_legacy_none(std::string_view text) {
  return trim(text).find_first_not_of('-') == std::string_view::npos;  // nothing, or nothing but hyphens
}

std::optional<std::string> take_name(Json& object, std::string_view key) {
  if (object.is_object())
    if (const auto it = object.find(key); it != object.end() && it->is_string()) {
      const auto& text = it->get_ref<const std::string&>();
      if (!text.empty() && is_legacy_none(text)) return std::nullopt;
    }
  return take_text(object, key);
}

std::optional<std::string> take_json(Json& object, std::string_view key) {
  if (!object.is_object()) return std::nullopt;
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  std::optional<std::string> text;
  if (!it->is_null()) text = dump(*it);
  object.erase(it);
  return text;
}

Json nonfinite_under(const std::vector<NonFinite>& nonfinite, std::string_view prefix) {
  Json out;
  for (const auto& n : nonfinite) {
    const std::string_view p = n.pointer;
    if (p.substr(0, prefix.size()) != prefix) continue;
    if (!prefix.empty() && p.size() > prefix.size() && p[prefix.size()] != '/') continue;  // "/Ar4" vs "/Ar40"
    out[std::string(p.substr(prefix.size()))] = n.token;
  }
  return out;
}

std::string pointer_of(std::string_view key) { return (Json::json_pointer() / std::string(key)).to_string(); }

Error unexpected_content(const std::exception& e) {
  return Error{ErrorKind::Protocol, std::string("unexpected content: ") + e.what(), {}};
}

std::optional<std::string> extra_text(const Json& object) {
  if (object.is_null() || (object.is_object() && object.empty())) return std::nullopt;
  return dump(object);
}

}  // namespace pychron::dvc
