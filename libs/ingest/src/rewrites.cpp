#include "rewrites.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace pychron::ingest::detail {

namespace {

constexpr std::string_view kSpace = " \t\r\n";
constexpr std::string_view kRewrites = "\"rewrites\"";
constexpr std::string_view kPath = "\"path\"";

std::string_view trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) return {};
  return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

// One past the JSON value that starts at `at`; npos when the text ends first.
std::size_t value_end(std::string_view text, std::size_t at) {
  if (at >= text.size()) return std::string_view::npos;
  const auto string_end = [&](std::size_t open) {
    for (std::size_t i = open + 1; i < text.size(); ++i) {
      if (text[i] == '\\')
        ++i;
      else if (text[i] == '"')
        return i + 1;
    }
    return std::string_view::npos;
  };
  if (text[at] == '"') return string_end(at);
  if (text[at] != '{' && text[at] != '[') {
    const auto end = text.find_first_of(",}] \t\r\n", at);
    return end == std::string_view::npos ? text.size() : end;
  }
  int depth = 0;
  for (std::size_t i = at; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') {
      i = string_end(i);
      if (i == std::string_view::npos) return i;
      --i;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (--depth == 0) return i + 1;
    }
  }
  return std::string_view::npos;
}

struct Member {
  std::string_view key;  // with its quotes
  std::string_view value;
};

// The values between the brackets of an object or array, split at the top
// level. false: not such a text.
bool split(std::string_view text, char open, char close, std::vector<std::string_view>& parts) {
  text = trimmed(text);
  if (text.size() < 2 || text.front() != open || text.back() != close) return false;
  std::size_t at = 1;
  const std::size_t last = text.size() - 1;
  while (true) {
    at = text.find_first_not_of(kSpace, at);
    if (at == std::string_view::npos || at >= last) return true;
    std::size_t end = at;
    if (open == '{') {  // "key" : value
      end = value_end(text, at);
      if (end == std::string_view::npos) return false;
      end = text.find_first_not_of(kSpace, end);
      if (end == std::string_view::npos || text[end] != ':') return false;
      end = text.find_first_not_of(kSpace, end + 1);
      if (end == std::string_view::npos) return false;
    }
    end = value_end(text, end);
    if (end == std::string_view::npos || end > last) return false;
    parts.push_back(text.substr(at, end - at));
    at = text.find_first_not_of(kSpace, end);
    if (at == std::string_view::npos || at >= last) return true;
    if (text[at] != ',') return false;
    ++at;
  }
}

bool members(std::string_view object, std::vector<Member>& out) {
  std::vector<std::string_view> parts;
  if (!split(object, '{', '}', parts)) return false;
  for (const auto part : parts) {
    const std::size_t key_end = value_end(part, 0);
    const std::size_t colon = part.find(':', key_end);
    out.push_back({part.substr(0, key_end), trimmed(part.substr(colon + 1))});
  }
  return true;
}

std::string quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

}  // namespace

std::optional<std::string> with_rewrites(std::string_view stored, std::string_view base,
                                         const std::vector<FileNote>& notes) {
  // The members to keep, and the entries already there, by quoted path.
  std::vector<Member> kept;
  std::vector<std::pair<std::string, std::string>> entries;
  std::vector<Member> all;
  const bool have_stored = !trimmed(stored).empty() && members(stored, all);
  if (!have_stored) {
    all.clear();
    (void)members(base, all);
  }
  for (const auto& member : all) {
    if (member.key != kRewrites) {
      kept.push_back(member);
      continue;
    }
    std::vector<std::string_view> elements;
    if (!split(member.value, '[', ']', elements)) continue;
    for (const auto element : elements) {
      std::vector<Member> fields;
      if (!members(element, fields)) continue;
      for (const auto& field : fields)
        if (field.key == kPath) entries.emplace_back(std::string(field.value), std::string(element));
    }
  }

  bool added = false;
  for (const auto& note : notes) {
    std::string path = quoted(note.path);
    const bool known = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == path; });
    if (known) continue;
    entries.emplace_back(std::move(path), note.json);
    added = true;
  }
  if (!added && have_stored) return std::nullopt;

  std::sort(entries.begin(), entries.end());
  std::string out = "{";
  for (const auto& member : kept) {
    out += member.key;
    out += ':';
    out += member.value;
    out += ',';
  }
  if (entries.empty()) {
    if (out.size() > 1) out.pop_back();
    return out + "}";
  }
  out += std::string(kRewrites) + ":[";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i > 0) out += ',';
    out += entries[i].second;
  }
  return out + "]}";
}

}  // namespace pychron::ingest::detail
