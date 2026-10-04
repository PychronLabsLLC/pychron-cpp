#pragma once

// The neutral unit a source adapter hands to the BatchWriter (legacy
// ingestion spec, section 2.1). A batch names things by source key (commit,
// path, git blob sha) and by natural key; the only uuids in it are analysis
// uuids. The writer derives every other id and resolves every natural key.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pychron/core/sha256.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/import.hpp"
#include "pychron/persistence/model.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::ingest {

// Where a row came from. `blob_sha` is the git blob sha of the file at
// `commit`, as text; for a non-git source any stable content key.
struct SourceKey {
  std::string commit, path, blob_sha;
};

struct GitWho {
  std::string name, email;
  persistence::UtcTime utc;  // author date
};

// ---------------------------------------------------------------- catalog
// Catalog items name their parents by natural key. A parent that is not in
// the store yet is created bare (key columns only), so every item is safe to
// repeat and to send before or after a restart.
//
// Order matters for the other columns: an existing row always wins, and the
// writer does not look at an item again once its row is known. Send the full
// item before anything that names it, or its columns stay empty:
//   PiItem before ProjectItem, SampleItem, PositionItem naming that PI;
//   SampleItem before a PositionItem naming the sample;
//   LevelItem before any PositionItem or RefObjectItem of that level;
//   PositionItem before a RefObjectItem scoped to that position, and before
//   a LoadPositionItem naming its identifier;
//   UserItem before a LoadItem naming that user;
//   LoadItem before a LoadPositionItem of that load;
//   MassSpecItem before a SpecialIdentifierItem or RefObjectItem naming it;
//   RefObjectItem before a LevelItem or LoadItem naming it as holder, and
//   before a revision whose subject is its RefObjectKey.

struct PiItem {
  std::string last_name, first_initial;
  std::optional<std::string> affiliation, email;
};

// A project's natural key is (name, principal investigator).
struct ProjectItem {
  std::string name;
  std::optional<std::string> pi_last_name, pi_first_initial;
  std::optional<std::string> checkin_date = std::nullopt;  // "YYYY-MM-DD"
  std::optional<std::string> comment = std::nullopt, lab_contact = std::nullopt, institution = std::nullopt;
};

struct MaterialItem {
  std::string name, grainsize;
};

struct SampleItem {
  // name, the descriptive columns and the two times; project, material and uuid are ignored
  persistence::SampleSpec fields;
  std::string project, material, grainsize;
  std::optional<std::string> pi_last_name, pi_first_initial;  // of the project
};

struct IrradiationItem {
  std::string name;
  std::optional<persistence::UtcTime> created = std::nullopt;  // the write time when unset
};

struct LevelItem {
  std::string irradiation, name;
  std::optional<std::string> holder;  // key of a ref_object of type irradiation_holder
  std::optional<double> z;
  std::optional<std::string> note;
};

// An irradiation position and, when `identifier` is not empty, the unknown
// identifier that sits in it. `sample` needs `project` and `material`.
struct PositionItem {
  std::string irradiation, level;
  int position = 0;
  std::string identifier;
  std::optional<std::string> sample, project, material, grainsize;
  std::optional<std::string> pi_last_name, pi_first_initial;  // of the project
  std::optional<double> weight = std::nullopt;
  std::optional<std::string> packet = std::nullopt, note = std::nullopt;
};

struct SpecialIdentifierItem {
  std::string identifier, analysis_type;
  std::optional<std::string> mass_spectrometer;
};

struct UserItem {
  std::string name;
  std::optional<std::string> email = std::nullopt, affiliation = std::nullopt, category = std::nullopt;
};

struct MassSpecItem {
  persistence::MassSpectrometerSpec spec;  // uuid ignored
};

struct ExtractDeviceItem {
  std::string name;
};

struct LoadItem {
  persistence::LoadSpec spec;              // holder and uuid ignored
  std::optional<std::string> holder_name;  // key of a ref_object of type load_holder
  // A user name; when set it replaces spec.created_by_user.
  std::optional<std::string> created_by = std::nullopt;
};

// One tray position of a load: the identifier loaded into it.
struct LoadPositionItem {
  std::string load;
  int position = 0;
  std::string identifier;
  std::optional<double> weight;
  std::optional<int> nxtals;
  std::optional<std::string> note;
};

struct RepositoryItem {
  std::string name;
};

// A reference object. Its scope is named, not given by uuid as in
// persistence::RefObjectSpec.
struct RefObjectItem {
  persistence::RefType type = persistence::RefType::Document;
  std::string key;
  std::optional<std::string> irradiation;
  std::optional<std::string> level;  // needs irradiation
  std::optional<int> position;       // needs level
  std::optional<std::string> mass_spectrometer;
};

// An interpreted age. `key` is the path of its file in the source; with the
// source url it is the object's identity (ids.hpp, interpreted_age_id), so the
// item is safe to repeat. It differs from persistence::InterpretedAgeSpec in
// naming its identifier and repository, as every catalog item does. An
// `identifier` no analysis in the store uses is left unset rather than
// created; the writer therefore writes these items after the batch's analyses. Send it before a revision whose subject is its InterpretedAgeKey;
// otherwise the object is created bare, named after its key.
struct InterpretedAgeItem {
  std::string key, name;
  std::optional<std::string> identifier, repository;
};

using CatalogItem = std::variant<PiItem, ProjectItem, MaterialItem, SampleItem, IrradiationItem, LevelItem, PositionItem,
                                 SpecialIdentifierItem, UserItem, MassSpecItem, ExtractDeviceItem, LoadItem,
                                 RepositoryItem, RefObjectItem, InterpretedAgeItem, LoadPositionItem>;

// ---------------------------------------------------------------- history

struct RefObjectKey {
  std::string ref_type;  // stored spelling, e.g. "flux_position"
  std::string name;      // the object's key
};

struct InterpretedAgeKey {
  std::string name;  // the InterpretedAgeItem's key
};

// What a revision is about: an analysis uuid, a reference object, or an
// interpreted age.
using SubjectRef = std::variant<persistence::Uuid, RefObjectKey, InterpretedAgeKey>;

// The file each root revision of an analysis came from. A kind the source has
// no file for is left empty (its root revision is then keyed by the record).
struct RootKeys {
  SourceKey record, signals, intercepts, baselines, blanks, icfactors, tags;
};

struct AnalysisItem {
  // Set: analysis uuid, identity, catalog names, rows of the roots, satellites.
  // Left for the writer: changeset, created, roots.* uuids, import_source,
  // author_user. An empty `analyst` becomes the author's user name.
  persistence::AnalysisIngest ingest;
  RootKeys keys;  // record: the <runid>.json commit, which dates and authors the collection
  GitWho who;
  bool synthetic_collection = false;
  std::vector<std::string> repositories;  // made a member of each
  std::string detail_json = "{}";         // a JSON object, kept in the analysis provenance row
};

// An analysis that is already in the store (imported from another source)
// and is seen in this one: it joins the repositories and nothing else.
struct MembershipItem {
  persistence::Uuid analysis;
  SourceKey key;  // its record file in this source
  GitWho who;
  std::vector<std::string> repositories;
};

struct BlobItem {
  SourceKey key;
  persistence::BlobIngest blob;
};

// A payload may name other analyses: the references of blank and IC-factor
// rows (`ref_analysis`) and the members of an interpreted age. Those are
// foreign keys, and the analysis may live in a source that is not imported
// yet. The writer therefore clears a `ref_analysis`, and drops a member, whose
// analysis is not in the store when the revision is written, and lists the
// uuids under "unresolved_references" in the revision's provenance detail
// (for the roots of an AnalysisItem, in the analysis's). It is not a conflict;
// the adapter keeps the reference verbatim in the row's extra or document.
struct RevisionItem {
  SourceKey key;
  SubjectRef subject;
  persistence::Kind kind = persistence::Kind::Intercepts;
  persistence::RevisionPayload payload;
  std::string detail_json = "{}";  // a JSON object, kept in the revision's provenance row when not empty
  // An Identity revision names its identifier here, as everything in a batch
  // is named; the writer sets IdentityValue::identifier from it. An
  // identifier that is not in the store makes the revision an
  // unknown_analysis conflict (retried by a replay); a run identity another
  // analysis has makes it an identity_clash conflict.
  std::string identifier = {};
  // A level_production revision names the production its level uses here: the
  // key of a ref_object of type production. The writer sets
  // LevelProductionValue::production from it, creating the object bare when
  // the store does not have it. Set on any other payload it is an error.
  std::string production_key = {};
  // The place of the revision's commit in the source's walk order as the
  // adapter has it now (ISourceAdapter::order_of of key.commit). The writer
  // does not write a revision behind one a later commit left (spec 10.34).
  // nullopt: the adapter has no order, and the revision is not compared.
  std::optional<std::int64_t> order = std::nullopt;
};

// A file a commit rewrote that is not revisioned (an analysis record or
// satellite file): `json` is a JSON object that has "path" among its members.
struct FileNote {
  std::string path, json;
};

struct ChangesetItem {
  std::string commit;
  persistence::ChangesetKind kind = persistence::ChangesetKind::Import;  // Import or Reference
  GitWho who;
  std::string message;
  std::vector<RevisionItem> revisions;  // may be empty: a commit recorded for its detail alone
  std::string detail_json = "{}";       // a JSON object, kept in the changeset's provenance row when not empty
  // Kept under "rewrites" in the same row, one entry per path, sorted by
  // path. The notes of one commit may arrive in several batches: the writer
  // adds those the row does not have yet.
  std::vector<FileNote> rewrites = {};
};

struct ConflictItem {
  SourceKey key;
  std::optional<persistence::Uuid> entity;
  persistence::ConflictKind kind = persistence::ConflictKind::Unparseable;
  std::optional<Sha256Digest> file_sha256;  // SHA-256 of the file bytes; set whenever the conflict is about a file
  std::string detail_json = "{}";
};

// A git tag: a bookmark of the heads of `analyses` at the moment the batch
// that carries it has been written, so that batch must end at the tagged
// commit. A tag none of whose analyses is in the store is skipped.
struct BookmarkItem {
  std::string name, commit;
  std::vector<persistence::Uuid> analyses;
  GitWho who;
};

struct ImportBatch {
  std::vector<CatalogItem> catalog;  // in dependency order
  std::vector<BlobItem> blobs;
  std::vector<AnalysisItem> analyses;
  std::vector<MembershipItem> memberships;
  std::vector<ChangesetItem> changesets;  // in source order
  std::vector<ConflictItem> conflicts;
  std::vector<BookmarkItem> bookmarks;
  std::string resume_token;  // valid once this batch is committed
  int done = 0, total = 0;
  std::string head;  // the source head this batch was read at; empty: as described
};

}  // namespace pychron::ingest
