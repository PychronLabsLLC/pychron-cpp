#pragma once

// Creating, publishing and withdrawing a release in the store (data
// publishing design, sections 4.3 and 4.4). A release is an immutable set of
// pinned revisions plus metadata; nothing is public by default.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/publish.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::publish {

using persistence::Uuid;
using persistence::UtcTime;

// The licence the New release dialog and `elctl publish create` preselect
// (spec 6.3). The owner may change it before the Publish page ships.
inline constexpr std::string_view kDefaultLicence = "CC-BY-4.0";

struct Author {
  std::string name;
  std::optional<std::string> orcid, affiliation;
  friend bool operator==(const Author&, const Author&) = default;
};

// JSON of an authors list, as `release.authors` stores it, and back.
std::string authors_json(const std::vector<Author>& authors);
Result<std::vector<Author>> parse_authors(std::string_view json);

struct ReleaseSpec {
  std::string name, title;
  std::optional<std::string> description;
  std::vector<Author> authors;
  std::string licence{kDefaultLicence};
  std::optional<std::string> publisher;
  std::optional<UtcTime> embargo_until;
  std::optional<std::string> supersedes;  // a release name ("r" or "r@v2") or a uuid; nullopt: the newest of `name`
  // Candidate analyses (the command's selection, already filtered by type
  // and --include-invalid), as uuid text.
  std::vector<std::string> analyses;
  // Head tag names kept; empty: every tag but "invalid".
  std::vector<std::string> tags;
  // Names of interpreted ages; nullopt: every one whose members are all in
  // the release; an empty vector: none.
  std::optional<std::vector<std::string>> interpreted_ages;
  std::string options_json, reduction_version, software_version;
};

struct ReleaseCreated {
  Uuid release;
  int version = 1;
  std::size_t members = 0, samples = 0, interpreted_ages = 0, embargoed_samples = 0, embargoed_analyses = 0;
};

// Spec 4.3: filters the candidates by head tag, resolves every member's
// references, makes a group and a bookmark pinning every head of every kind,
// every resolved reference at its revision and every chosen interpreted age
// at its head, then the release row (state draft, visibility private) with
// its samples and interpreted ages. Version is one more than the newest of
// the name. Config when no candidate is left.
Result<ReleaseCreated> create_release(persistence::IStore& store, const persistence::Actor& actor,
                                      const ReleaseSpec& spec);

// draft -> published, recording the manifest of the published build; public
// when asked, else the release stays private (spec R1).
Result<void> publish_release(persistence::IStore& store, const persistence::Actor& actor, Uuid release,
                             const Sha256Digest& manifest_sha256, bool make_public);
Result<void> withdraw_release(persistence::IStore& store, const persistence::Actor& actor, Uuid release,
                              std::string reason);
Result<void> set_release_public(persistence::IStore& store, const persistence::Actor& actor, Uuid release);
Result<void> delete_release(persistence::IStore& store, const persistence::Actor& actor, Uuid release);

// "name", "name@v2" or a uuid text -> the row; Config when none.
Result<persistence::ReleaseRow> find_release(persistence::IStore& store, std::string_view name_or_uuid);

}  // namespace pychron::publish
