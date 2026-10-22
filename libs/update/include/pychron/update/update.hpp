// Telling the user that a newer release exists.
//
// This library reads release versions and GitHub's "latest release" answer.
// It has no Qt and no network code: all I/O goes through http::IHttpClient
// (libs/core), which a program gives it. Nothing here downloads or installs.
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/core/http.hpp"

namespace pychron::update {

// The repository releases are read from; never a preference.
inline constexpr std::string_view kRepository = "PychronLabsLLC/pychron-cpp";

struct Version {
  int major = 0, minor = 0, patch = 0;
  auto operator<=>(const Version&) const = default;
};

// "0.5.0", without a leading "v".
std::string to_string(Version version);

struct Release {
  Version version;
  std::string tag, html_url, notes;
};

// "v1.2.3" or "1.2.3": three numbers, nothing before (but one "v") or after.
// Pre-release and build suffixes are refused.
[[nodiscard]] Result<Version> parse_version(std::string_view text);

// The body of GitHub's releases/latest. A draft or a pre-release, a missing
// tag_name or html_url, and a tag that is not a version are errors; a missing
// or null body is empty notes. Unknown fields are ignored.
[[nodiscard]] Result<Release> parse_latest_release(std::string_view json);

enum class FetchOutcome { Found, NoRelease };

struct Fetched {
  FetchOutcome outcome;
  std::optional<Release> release;  // set only for Found
};

// One GET of releases/latest of `repository` ("owner/name") through `client`.
// 200 is Found, 404 is NoRelease (the repository has none yet); any other
// status, an unreadable 200 and every client error are errors (a bad status
// or body is Protocol, a client error is passed through as it came).
[[nodiscard]] Result<Fetched> fetch_latest(http::IHttpClient& client, std::string_view repository = kRepository);

}  // namespace pychron::update
