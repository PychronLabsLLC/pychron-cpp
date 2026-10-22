#include "pychron/update/update.hpp"

#include <cctype>
#include <charconv>
#include <string>

#include <nlohmann/json.hpp>

namespace pychron::update {

using json = nlohmann::json;

std::string to_string(Version version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
}

Result<Version> parse_version(std::string_view text) {
  const std::string shown(text);
  const auto bad = [&shown] { return fail(ErrorKind::Protocol, "update: \"" + shown + "\" is not a version (x.y.z)"); };
  if (!text.empty() && text.front() == 'v') text.remove_prefix(1);
  int parts[3] = {0, 0, 0};  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): fixed three
  for (int i = 0; i < 3; ++i) {
    std::size_t n = 0;
    while (n < text.size() && std::isdigit(static_cast<unsigned char>(text[n])) != 0) ++n;
    if (n == 0) return bad();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the range is [data, data + n), n bounded by size
    const auto [end, ec] = std::from_chars(text.data(), text.data() + n, parts[i]);
    if (ec != std::errc{} || end != text.data() + n) return bad();
    text.remove_prefix(n);
    if (i < 2) {
      if (text.empty() || text.front() != '.') return bad();
      text.remove_prefix(1);
    }
  }
  if (!text.empty()) return bad();
  return Version{parts[0], parts[1], parts[2]};
}

namespace {

Unexpected<Error> bad(const std::string& what) { return fail(ErrorKind::Protocol, "update: latest release: " + what); }

}  // namespace

Result<Release> parse_latest_release(std::string_view text) {
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded()) return bad("it is not JSON");
  if (!doc.is_object()) return bad("it is not a JSON object");

  const auto flag = [&doc](const char* key) {
    const auto it = doc.find(key);
    return it != doc.end() && it->is_boolean() && it->get<bool>();
  };
  if (flag("draft")) return bad("it is a draft");
  if (flag("prerelease")) return bad("it is a pre-release");

  const auto string_field = [&doc](const char* key) -> std::string {
    const auto it = doc.find(key);
    return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string{};
  };
  Release release;
  release.tag = string_field("tag_name");
  if (release.tag.empty()) return bad("tag_name is missing");
  release.html_url = string_field("html_url");
  if (release.html_url.empty()) return bad("html_url is missing");
  release.notes = string_field("body");

  const auto version = parse_version(release.tag);
  if (!version) return bad("tag_name \"" + release.tag + "\" is not a version");
  release.version = *version;
  return release;
}

}  // namespace pychron::update
