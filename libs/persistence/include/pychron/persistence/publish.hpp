#pragma once

// Releases, embargoes and registered identifiers (data publishing and hosted
// sharing design, section 4). Plain value types; the store's methods are on
// IStore (store.hpp, "publishing").
//
// A release pins revisions through a bookmark on an analysis_group; its own
// row changes only in its state columns, and its samples and interpreted ages
// are frozen with it. An embargo hides a project, a sample or an analysis
// from every public surface until a date or until lifted. An external id is
// the receipt of a DOI or IGSN registration; sample.igsn and release.doi are
// the catalog's copies.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/persistence/ids.hpp"

namespace pychron::persistence {

enum class ReleaseState { Draft, Published, Withdrawn };
enum class Visibility { Private, Public };
enum class EmbargoScopeType { Project, Sample, Analysis };

// Database spellings ("draft", "private", "sample", ...).
std::string_view to_string(ReleaseState state) noexcept;
std::string_view to_string(Visibility visibility) noexcept;
std::string_view to_string(EmbargoScopeType type) noexcept;
std::optional<ReleaseState> parse_release_state(std::string_view text) noexcept;
std::optional<Visibility> parse_visibility(std::string_view text) noexcept;
std::optional<EmbargoScopeType> parse_embargo_scope_type(std::string_view text) noexcept;

// ---------------------------------------------------------------- releases (4.2, 4.4)

struct ReleaseInsert {
  Uuid uuid;  // caller-generated v7
  std::string name, title;
  std::optional<std::string> description;
  int version = 1;
  std::optional<Uuid> supersedes;
  Uuid group, bookmark;
  std::string licence;       // SPDX id
  std::string authors_json;  // [{name, orcid?, affiliation?}]
  std::optional<std::string> publisher;
  std::optional<UtcTime> embargo_until;
  std::string options_json, reduction_version, software_version;
  std::vector<Uuid> samples;                             // release_sample rows
  std::vector<std::pair<Uuid, Uuid>> interpreted_ages;  // (ia_uuid, revision_uuid)
};

struct ReleaseRow {
  Uuid uuid;
  std::string name, title;
  std::optional<std::string> description;
  int version = 1;
  std::optional<Uuid> supersedes;
  Uuid group, bookmark;
  ReleaseState state = ReleaseState::Draft;
  Visibility visibility = Visibility::Private;
  std::string licence, authors_json;
  std::optional<std::string> publisher;
  std::optional<UtcTime> embargo_until;
  std::optional<std::string> doi, doi_state;
  std::string options_json, reduction_version, software_version;
  std::optional<Sha256Digest> manifest_sha256;
  Uuid created_by, client;
  UtcTime created;
  std::optional<UtcTime> published, withdrawn;
  std::optional<std::string> withdrawn_reason;
};

// A change of the columns a release may change (spec 4.4). Legal:
//   draft -> published (sets published_utc); published -> withdrawn (sets
//   withdrawn_utc; needs withdrawn_reason); published: private -> public;
//   public -> private only together with withdrawn. A draft's visibility
//   does not change; a withdrawn release never changes. Anything else is
//   refused with Config and nothing is written.
struct ReleaseUpdate {
  Uuid release;
  std::optional<ReleaseState> state;
  std::optional<Visibility> visibility;
  std::optional<std::string> doi, doi_state, withdrawn_reason;
  std::optional<Sha256Digest> manifest_sha256;
};

struct ReleaseQuery {
  std::optional<ReleaseState> state;
  std::optional<std::string> name;
};

struct ReleaseInterpretedAge {
  Uuid ia, revision;
  friend bool operator==(const ReleaseInterpretedAge&, const ReleaseInterpretedAge&) = default;
};

// An interpreted age of the catalog with its head revision (nullopt before
// its first value), for choosing the ones a release carries (spec 4.3).
struct InterpretedAgeRow {
  Uuid uuid;
  std::string name;
  std::optional<Uuid> identifier;
  std::string identifier_text;  // the identifier's labnumber, "" when none
  std::optional<Uuid> repository;
  std::optional<Uuid> head;
};

// ---------------------------------------------------------------- embargoes (4.2)

struct EmbargoScope {
  EmbargoScopeType type = EmbargoScopeType::Sample;
  Uuid uuid;
  friend bool operator==(const EmbargoScope&, const EmbargoScope&) = default;
};

struct EmbargoInsert {
  EmbargoScope scope;
  std::optional<UtcTime> until;  // nullopt: until lifted by hand
  std::optional<std::string> reason;
};

struct EmbargoRow {
  Uuid uuid;
  EmbargoScope scope;
  std::optional<UtcTime> until;
  std::optional<std::string> reason;
  Uuid set_by;
  UtcTime created;
  std::optional<UtcTime> lifted;
  std::optional<Uuid> lifted_by;
};

// ---------------------------------------------------------------- external ids (10.3)

struct ExternalIdInsert {
  std::string entity_type;  // release | sample
  Uuid entity;
  std::string scheme;     // doi | igsn
  std::string value;
  std::string registrar;  // datacite, datacite-test, sesar, sesar-sandbox
  std::string state;      // draft | registered | findable | in_doubt
  Sha256Digest request_sha256{};
  std::optional<std::string> response_json;
};

struct ExternalIdRow {
  Uuid uuid;
  std::string entity_type;
  Uuid entity;
  std::string scheme, value, registrar, state;
  Sha256Digest request_sha256{};
  std::optional<std::string> response_json;
  UtcTime registered;
};

}  // namespace pychron::persistence
