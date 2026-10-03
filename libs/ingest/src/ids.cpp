#include "pychron/ingest/ids.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>

namespace pychron::ingest {

using persistence::Uuid;

namespace {

void lower(std::string& text, std::size_t from, std::size_t to) {
  std::transform(text.begin() + static_cast<std::ptrdiff_t>(from), text.begin() + static_cast<std::ptrdiff_t>(to),
                 text.begin() + static_cast<std::ptrdiff_t>(from),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

void strip_trailing_slashes(std::string& text) {
  while (text.size() > 1 && (text.back() == '/' || text.back() == '\\')) text.pop_back();
}

void strip_git_suffix(std::string& text) {
  constexpr std::string_view suffix = ".git";
  strip_trailing_slashes(text);
  if (text.size() > suffix.size() && text.ends_with(suffix)) text.erase(text.size() - suffix.size());
  strip_trailing_slashes(text);
}

// Lower-cases the host of "[user@]host[:port]" at [from, to).
void lower_host(std::string& text, std::size_t from, std::size_t to) {
  const auto at = text.rfind('@', to == 0 ? 0 : to - 1);
  if (at != std::string::npos && at >= from) from = at + 1;
  lower(text, from, to);
}

Uuid derive(std::string_view tag, std::initializer_list<std::string_view> parts) {
  static const Uuid ns = *Uuid::parse(kImportNamespace);
  std::string name(tag);
  for (auto part : parts) {
    name += '\n';
    name += part;
  }
  return Uuid::v5(ns, name);
}

}  // namespace

std::string normalize_source_url(std::string_view url) {
  std::string out(url);

  if (const auto scheme = out.find("://"); scheme != std::string::npos) {
    strip_git_suffix(out);
    lower(out, 0, scheme);
    const auto authority = scheme + 3;
    const auto path = out.find('/', authority);
    lower_host(out, authority, path == std::string::npos ? out.size() : path);
    return out;
  }

  // scp-like "user@host:path". A one-letter "host" is a Windows drive.
  const auto colon = out.find(':');
  const auto slash = out.find_first_of("/\\");
  if (colon != std::string::npos && colon > 1 && (slash == std::string::npos || colon < slash)) {
    strip_git_suffix(out);
    lower_host(out, 0, colon);
    return out;
  }

  std::error_code ec;
  const auto absolute = std::filesystem::absolute(std::filesystem::path(out), ec);
  if (!ec) out = absolute.lexically_normal().string();
  strip_trailing_slashes(out);
  return out;
}

Uuid source_id(persistence::ImportSourceKind kind, std::string_view url, std::string_view branch) {
  return derive("source", {persistence::to_string(kind), url, branch});
}

Uuid changeset_id(std::string_view url, std::string_view commit) { return derive("changeset", {url, commit}); }

Uuid collection_changeset_id(std::string_view url, std::string_view commit, std::string_view path) {
  return derive("collection", {url, commit, path});
}

Uuid revision_id(std::string_view url, std::string_view commit, std::string_view path) {
  return derive("revision", {url, commit, path});
}

Uuid catalog_id(std::string_view table, std::string_view natural_key) {
  return derive("catalog", {table, natural_key});
}

Uuid derived_analysis_id(std::string_view url, std::string_view runid) { return derive("analysis", {url, runid}); }

Uuid conflict_id(std::string_view url, std::string_view commit, std::string_view path) {
  return derive("conflict", {url, commit, path});
}

}  // namespace pychron::ingest
