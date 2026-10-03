#pragma once

// The DVC store: one access layer over PostgreSQL (shared lab database) and
// SQLite (tests, offline export). DVC schema spec, section 12.2.
//
// Thread model: an IStore and every unit of work it hands out belong to the
// thread that opened the store. Open one store per thread for concurrent use.
// No driver exception crosses this interface; failures are Result errors and a
// lost compare-and-swap is a value (CommitOutcome holding conflicts).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/blob.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/import.hpp"
#include "pychron/persistence/model.hpp"

namespace pychron::persistence {

enum class Dialect { PostgreSql, Sqlite };

struct StoreConfig {
  // "sqlite::memory:", "sqlite:/path/to/file.db",
  // "postgresql://user:password@host:port/dbname[?sslmode=verify-full&sslrootcert=...&search_path=...]"
  std::string url;
  // Apply pending migrations on open. Off: refuse to open an outdated schema.
  bool migrate = true;
};

struct Actor {
  Uuid user;
  Uuid client;
};

// ---------------------------------------------------------------- writes (5.4)

struct Conflict {
  Uuid subject;
  Kind kind = Kind::Intercepts;
  std::optional<Uuid> expected;
  std::optional<Uuid> actual;
  std::optional<ChangesetInfo> actual_by;  // the changeset that moved the head to `actual`
};

struct Committed {
  Uuid changeset;
  ChangeSeq seq = 0;
};

// A lost CAS race is an expected outcome, not an Error. Nothing of a
// conflicted changeset persists.
using CommitOutcome = std::variant<Committed, std::vector<Conflict>>;

class IUnitOfWork {
 public:
  virtual ~IUnitOfWork() = default;

  // Stages a revision whose parent is `expected_head` and a head move from
  // `expected_head` to it (nullopt: the first head of this subject and kind).
  // Returns the revision's client-generated id.
  virtual Result<Uuid> add_revision(Uuid subject, Kind kind, RevisionPayload payload,
                                    std::optional<Uuid> expected_head) = 0;
  // Stages a head move to an existing revision (rollback, collection restore).
  virtual Result<void> move_head(Uuid subject, Kind kind, std::optional<Uuid> expected, Uuid to,
                                 MoveReason reason) = 0;
  // One transaction: changeset, revisions and payloads, CAS head moves in
  // (subject, kind) order, head_move rows, change cursor. Consumes the staging.
  virtual Result<CommitOutcome> commit(ChangesetKind kind, std::string message) = 0;
};

// ---------------------------------------------------------------- catalog

struct ClientRegistration {
  std::string hostname;
  std::string role;  // acquisition | reduction | viewer | publisher | importer | admin
  std::optional<Uuid> mass_spectrometer;
  std::string software_version;
};

struct MassSpectrometerSpec {
  std::string name;
  std::optional<std::string> kind;
  std::optional<std::string> code;
};

struct IdentifierSpec {
  std::string identifier;
  std::string kind = "unknown";               // unknown | special
  std::optional<std::string> analysis_type;  // required for special identifiers
  std::optional<Uuid> mass_spectrometer;
  std::optional<Uuid> position;              // irradiation_position (unknowns only)
  std::optional<Uuid> sample;                // when there is no irradiation position
};

struct PrincipalInvestigatorSpec {
  std::string last_name;
  std::string first_initial;
  std::optional<std::string> affiliation, email;
};

struct ProjectSpec {
  std::string name;
  std::optional<Uuid> principal_investigator;
};

struct MaterialSpec {
  std::string name;
  std::string grainsize;
};

struct SampleSpec {
  std::string name;
  Uuid project;
  Uuid material;
  std::optional<std::string> note, igsn;
  std::optional<double> lat, lon;
};

struct LevelSpec {
  Uuid irradiation;
  std::string name;
  std::optional<Uuid> holder;  // ref_object of type irradiation_holder
  std::optional<double> z;
  std::optional<std::string> note;
};

struct PositionSpec {
  Uuid level;
  int position = 0;
  std::optional<Uuid> sample;
  std::optional<double> weight;
  std::optional<std::string> packet, note;
};

// A reference object (section 6.1). `key` is unique per ref_type; the scope
// columns are what resolve_refs() matches an analysis against.
struct RefObjectSpec {
  RefType type = RefType::Document;
  std::string key;  // "<irrad>/<level>/<pos>", "<irrad>", "<ms>", ...
  std::optional<Uuid> irradiation, level, position, mass_spectrometer;
};

struct InterpretedAgeSpec {
  std::string name;
  std::optional<Uuid> identifier;
  std::optional<Uuid> repository;
};

// Bookmarks capture the heads of every analysis in a repository or a group
// (section 5.5). Exactly one scope must be set.
struct BookmarkSpec {
  std::string name;
  std::optional<std::string> message;
  std::optional<Uuid> repository;
  std::optional<Uuid> group;
};

// ---------------------------------------------------------------- ingest (5.3, 8.3)

struct IsotopeRow {
  std::string isotope;
  std::string detector;
  std::optional<std::string> units, detector_serial, classification;
  std::optional<double> classification_probability;
};

struct DetectorRow {
  std::string detector;
  std::optional<double> deflection, gain_used;
};

struct ExtractionFields {
  std::optional<double> extract_value;
  std::optional<std::string> extract_units;
  std::optional<double> extract_duration, cleanup_duration, pre_cleanup, post_cleanup, cryo_temperature, weight,
      beam_diameter;
  std::optional<std::string> pattern;
  std::optional<double> ramp_duration, ramp_rate, light_value;
  std::optional<std::string> tray;
};

// analysis_meta: JSON columns that are never filtered on.
struct AnalysisMetaRow {
  std::optional<std::string> source_json, environmental_json, conditionals_json, tripped_conditional_json,
      whiff_result_json;
  std::optional<double> intensity_scalar;
  std::optional<std::string> baseline_modifiers_json, arar_mapping_json, extraction_context_json, pid_json,
      snapshots_json, videos_json, grain_polygons_json, pipette_counts_json, software_json, queue_names_json,
      legacy_json;
};

struct PeakCenterRow {
  std::string detector;
  std::optional<std::string> reference_detector, reference_isotope, interpolation;
  std::optional<double> low_dac, center_dac, high_dac, low_signal, center_signal, high_signal, resolution,
      low_resolving_power, high_resolving_power;
  std::optional<Sha256Digest> points_blob_sha;
};

struct MonitorCheckRow {
  int ordinal = 0;
  std::optional<std::string> name, parameter, criterion, comparator;
  std::optional<bool> tripped;
  std::optional<Sha256Digest> data_blob_sha;
};

struct ArtifactRow {
  std::string name;
  std::string kind;  // log | snapshot | video | stream | other
  std::optional<Sha256Digest> blob_sha;
  std::optional<std::string> url;  // blob_sha or url is required
};

struct MeasuredPositionRow {
  std::optional<std::string> load_name;
  std::optional<int> position;
  std::optional<double> x, y, z;
  bool is_degas = false;
};

// Content-addressed: sha256 = SHA-256 of the four JSON texts joined by NUL.
struct SpectrometerSnapshot {
  std::optional<std::string> legacy_sha1;
  std::string spectrometer_json = "{}", gains_json = "{}", deflections_json = "{}", settings_json = "{}";
};
Sha256Digest snapshot_sha256(const SpectrometerSnapshot& snapshot);

// Script texts used by the run, stored once in script_text by SHA-256.
struct ScriptBodies {
  std::optional<std::string> measurement, extraction, post_equilibration, post_measurement, hops;
};

struct QueueRow {
  Uuid uuid;
  std::string name;
  std::optional<std::string> creator;  // app_user name
  std::optional<Sha256Digest> text_blob_sha;
  std::optional<int> schema_version;
};

// The root revisions of a new analysis, one per collection kind (I3).
struct CollectionRoots {
  Uuid signals, intercepts, baselines, blanks, icfactors, tags;
  SignalRefs signal_refs;
  Intercepts intercepts_rows;
  Baselines baselines_rows;
  Blanks blanks_rows;
  IcFactors icfactors_rows;
  TagValue tag{"ok", std::nullopt, std::nullopt};
};

// An acquired analysis. Catalog rows are named by natural key (section 3.8);
// every server row id is generated by the acquisition client, so re-sending
// the same item cannot create a second copy of anything.
struct AnalysisIngest {
  Uuid analysis;
  Uuid changeset;
  UtcTime created;  // client clock, stamped on the changeset and revisions

  std::string identifier;
  int aliquot = 0;
  int increment = -1;  // -1: no step
  std::string analysis_type;
  std::optional<std::string> experiment_type;
  UtcTime timestamp;
  std::optional<UtcTime> time_zero;
  std::string mass_spectrometer;
  std::optional<std::string> extract_device;
  ExtractionFields extraction;
  std::optional<std::string> load_name, load_holder;
  std::optional<int> run_index;
  std::optional<std::string> laboratory, instrument_name;
  std::string analyst;  // app_user name, created if missing
  // Set by an importer only.
  std::optional<Uuid> import_source;  // stamped on the collection changeset
  std::optional<Uuid> author_user;    // overrides `analyst` as changeset author when set
  std::optional<Sha256Digest> record_sha256;
  int record_schema_version = 1;

  std::vector<IsotopeRow> isotopes;
  std::vector<DetectorRow> detectors;
  CollectionRoots roots;

  // Satellites (section 3.5). Blob references in peak centers, monitor checks
  // and artifacts count towards signals_state like signal refs (I13).
  std::optional<AnalysisMetaRow> meta;
  std::vector<PeakCenterRow> peak_centers;
  std::vector<MonitorCheckRow> monitor_checks;
  std::vector<ArtifactRow> artifacts;
  std::vector<MeasuredPositionRow> measured_positions;
  std::optional<SpectrometerSnapshot> spectrometer_snapshot;
  ScriptBodies scripts;
  std::optional<QueueRow> queue;
};

// One content-addressed raw series.
struct BlobIngest {
  std::string codec;
  Bytes bytes;
  std::optional<int> n_points;
};

struct IngestItem {
  Uuid item;                    // idempotency key
  Sha256Digest payload_sha256;  // of the canonical outbox payload
  Uuid client;
  std::variant<AnalysisIngest, BlobIngest> body;
};

struct IngestAck {
  std::optional<ChangeSeq> change_seq;  // nullopt when nothing changed (a blob already present)
  bool duplicate = false;               // the item had already been ingested
};

// ---------------------------------------------------------------- reads

struct AnalysisSummary {
  Uuid uuid;
  std::string runid;
  std::string identifier;
  int aliquot = 0;
  int increment = -1;
  bool provisional = false;
  std::string analysis_type;
  UtcTime timestamp;
  std::string mass_spectrometer;
  std::string signals_state;  // pending | complete
};

struct AnalysisQuery {
  std::optional<std::string> identifier;
  std::optional<std::string> mass_spectrometer;
  std::optional<std::string> analysis_type;
  std::optional<UtcTime> from, to;  // timestamp_utc range, inclusive
  int limit = 500;
};

struct AnalysisView {
  AnalysisSummary summary;
  std::vector<HeadInfo> heads;
  std::map<Kind, RevisionPayload> payloads;  // at the heads
};

struct ChangeEntity {
  std::string entity_type;
  Uuid entity;
  std::string op;
};

struct ChangeEntry {
  ChangeSeq seq = 0;
  UtcTime committed;
  // Null for a "changeset" entry written by an import batch, which holds
  // several changesets: they are among `entities` (entity_type "changeset").
  std::optional<Uuid> changeset;
  Uuid client;
  std::string kind;  // changeset | ingest | blob_complete | catalog | lease
  std::vector<ChangeEntity> entities;
};

struct ChangePage {
  std::vector<ChangeEntry> entries;
  ChangeSeq cursor = 0;  // the last seq in this page, or the input cursor if empty
  bool more = false;
};

// ---------------------------------------------------------------- reference resolution (6.2)

struct RefPolicy {
  bool honour_pins = true;  // false: every reference at head
};

struct ResolvedRef {
  Uuid ref_object;
  RefType type = RefType::Document;
  std::string key;
  Uuid revision;
  bool pinned = false;
};

// The reference revisions a reduction of one analysis uses: flux of its
// irradiation position, level geometry, level production and the production
// it names, the irradiation chronology, and the spectrometer's gains and
// sensitivity. A reference that does not exist yet is simply absent.
struct RefResolution {
  std::vector<ResolvedRef> refs;  // ordered by ref_object
};

// ---------------------------------------------------------------- derived cache (4.3)

struct DerivedRow {
  std::string name;  // e.g. "age", "kca"
  std::optional<double> value, error;
  std::optional<std::string> units;
  friend bool operator==(const DerivedRow&, const DerivedRow&) = default;
};

struct AppliedMigration {
  int version = 0;
  std::string description;
  std::string checksum_hex;
};

// ---------------------------------------------------------------- browsing
// (data browsing and visualization design, section 9.2)

// Every list is "any of"; empty lists do not filter. Sample, project, PI and
// material come from the identifier's sample or its irradiation position's.
struct BrowseFilter {
  std::string text;  // case-insensitive prefix of run id, identifier or sample
  std::vector<std::string> identifiers, samples, projects, principal_investigators, materials, analysis_types,
      mass_spectrometers, extract_devices, loads, irradiations, levels, repositories;
  std::optional<UtcTime> from, to;    // timestamp_utc, inclusive
  std::optional<double> last_hours;   // relative to the newest analysis in the database
  std::vector<std::string> exclude_tags;  // names of the head tag
};

struct BrowseCursorKey {
  UtcTime timestamp;
  Uuid uuid;
  friend bool operator==(const BrowseCursorKey&, const BrowseCursorKey&) = default;
};

struct BrowseRequest {
  BrowseFilter filter;
  int limit = 200;
  std::optional<BrowseCursorKey> after;  // rows strictly older than this
  bool count_total = true;
};

struct BrowseRow {
  AnalysisSummary summary;
  std::string sample, project, material, principal_investigator, extract_device, load, irradiation, level,
      repository, tag;
  std::optional<int> position;
  std::optional<double> extract_value;
  std::string extract_units;
};

struct BrowseResult {
  std::vector<BrowseRow> rows;  // newest first
  std::optional<BrowseCursorKey> next;
  std::optional<std::int64_t> total;
};

enum class BrowseFacet {
  AnalysisType,
  MassSpectrometer,
  ExtractDevice,
  Project,
  PrincipalInvestigator,
  Sample,
  Material,
  Identifier,
  Irradiation,
  Level,
  Load,
  Repository,
};

// What recall and reduction need beyond the head payloads.
struct AnalysisDetail {
  BrowseRow row;
  ExtractionFields extraction;
  std::vector<IsotopeRow> isotopes;
  std::vector<DetectorRow> detectors;
  std::vector<PeakCenterRow> peak_centers;
  std::optional<std::string> analyst;
  std::optional<std::string> environmental_json;  // analysis_meta.environmental
};

struct BlobData {
  std::string codec;
  Bytes bytes;
  std::optional<int> n_points;
};

class IStore {
 public:
  virtual ~IStore() = default;

  virtual Dialect dialect() const noexcept = 0;
  virtual Result<std::vector<AppliedMigration>> schema_status() = 0;

  virtual Result<std::unique_ptr<IUnitOfWork>> begin(const Actor& actor) = 0;

  // Import bookkeeping (legacy ingestion). begin_import inserts the source or
  // returns the stored row; begin_import_batch stages one batch of that
  // source's history, written as `client`; the rest are reads.
  virtual Result<ImportSourceInfo> begin_import(const ImportSourceSpec& spec) = 0;
  virtual Result<std::unique_ptr<IImportUnitOfWork>> begin_import_batch(Uuid source, Uuid client) = 0;
  virtual Result<std::vector<ImportSourceInfo>> import_sources() = 0;
  virtual Result<std::vector<ImportConflictRow>> import_conflicts(const ConflictFilter& filter) = 0;
  virtual Result<std::vector<ProvenanceRow>> provenance_for(Uuid entity) = 0;
  virtual Result<bool> has_provenance(Uuid source, std::string_view commit_sha, std::string_view path) = 0;
  virtual Result<bool> has_provenance_blob(Uuid source, std::string_view path, std::string_view git_blob_sha) = 0;
  virtual Result<bool> has_conflict(Uuid source, std::string_view path, const Sha256Digest& file_sha256) = 0;
  // The git blob sha recorded for the current head revision of (subject, kind)
  // from this source; nullopt if none.
  virtual Result<std::optional<std::string>> imported_head_blob_sha(Uuid source, Uuid subject, Kind kind) = 0;

  // Catalog (not revisioned; every write is in change_entity with a field diff, D6).
  virtual Result<Uuid> register_client(const ClientRegistration& registration) = 0;
  virtual Result<Uuid> ensure_user(Uuid client, const std::string& name) = 0;
  virtual Result<Uuid> add_mass_spectrometer(Uuid client, const MassSpectrometerSpec& spec) = 0;
  virtual Result<Uuid> add_identifier(Uuid client, const IdentifierSpec& spec) = 0;
  virtual Result<Uuid> add_extract_device(Uuid client, const std::string& name) = 0;
  virtual Result<Uuid> add_principal_investigator(Uuid client, const PrincipalInvestigatorSpec& spec) = 0;
  virtual Result<Uuid> add_project(Uuid client, const ProjectSpec& spec) = 0;
  virtual Result<Uuid> add_material(Uuid client, const MaterialSpec& spec) = 0;
  virtual Result<Uuid> add_sample(Uuid client, const SampleSpec& spec) = 0;
  virtual Result<Uuid> add_irradiation(Uuid client, const std::string& name) = 0;
  virtual Result<Uuid> add_level(Uuid client, const LevelSpec& spec) = 0;
  virtual Result<Uuid> add_irradiation_position(Uuid client, const PositionSpec& spec) = 0;
  // A reference object; its values are `value` revisions staged through a unit
  // of work with Kind::RefValue and a RefPayload matching its type.
  virtual Result<Uuid> add_ref_object(Uuid client, const RefObjectSpec& spec) = 0;
  // An interpreted age; its values are Kind::InterpretedAge revisions.
  virtual Result<Uuid> add_interpreted_age(Uuid client, const InterpretedAgeSpec& spec) = 0;

  // Groups, repositories, bookmarks (sections 3.6, 5.5).
  virtual Result<Uuid> add_repository(Uuid client, const std::string& name) = 0;
  virtual Result<void> add_repository_members(const Actor& actor, Uuid repository,
                                              const std::vector<Uuid>& analyses) = 0;
  virtual Result<Uuid> create_group(const Actor& actor, const std::string& name,
                                    const std::vector<Uuid>& analyses) = 0;
  virtual Result<Uuid> create_bookmark(const Actor& actor, const BookmarkSpec& spec) = 0;
  virtual Result<std::vector<HeadInfo>> bookmark_heads(Uuid bookmark) = 0;
  // CAS moves of every head in the bookmark that differs from it, as one
  // bookmark_restore changeset. Committed{nil, 0} when nothing differs.
  virtual Result<CommitOutcome> restore_bookmark(const Actor& actor, Uuid bookmark, std::string message) = 0;
  // Moves the given kinds' heads (default: the six collection kinds) back to
  // the analysis's collection revisions, as one rollback changeset.
  virtual Result<CommitOutcome> rollback_to_collection(const Actor& actor, Uuid analysis, std::string message,
                                                       std::vector<Kind> kinds = {}) = 0;

  // Reference resolution and the derived cache.
  virtual Result<RefResolution> resolve_refs(Uuid analysis, const RefPolicy& policy) = 0;
  // SHA-256 over the analysis's head revisions, its resolved reference
  // revisions and the reduction version (section 4.3).
  virtual Result<Sha256Digest> input_fingerprint(Uuid analysis, const std::string& reduction_version) = 0;
  virtual Result<void> put_derived(Uuid analysis, const Sha256Digest& fingerprint,
                                   const std::string& reduction_version, const std::vector<DerivedRow>& rows) = 0;
  // Cached rows only if computed from the current inputs (I14); else nullopt.
  virtual Result<std::optional<std::vector<DerivedRow>>> get_derived(Uuid analysis,
                                                                    const std::string& reduction_version) = 0;
  // Deletes cache rows whose inputs are no longer current. Returns rows removed.
  virtual Result<int> prune_derived(Uuid analysis) = 0;

  // Idempotent ingest of one outbox item (I7). Used by the uploader.
  virtual Result<IngestAck> ingest(const IngestItem& item) = 0;

  virtual Result<std::optional<Uuid>> head(Uuid subject, Kind kind) = 0;
  virtual Result<std::vector<HeadInfo>> heads(Uuid subject) = 0;
  // Every revision of (subject, kind), oldest first by change_seq. An
  // imported revision takes the change_seq of the batch that stored its
  // changeset; revisions sharing a change_seq are listed parent before child.
  virtual Result<std::vector<RevisionInfo>> history(Uuid subject, Kind kind) = 0;
  virtual Result<std::optional<RevisionPayload>> load_payload(Uuid revision) = 0;
  virtual Result<std::optional<AnalysisView>> load_analysis(Uuid analysis) = 0;
  virtual Result<std::vector<AnalysisSummary>> find_analyses(const AnalysisQuery& query) = 0;
  // Newest first, keyset paged on (timestamp, uuid).
  virtual Result<BrowseResult> browse(const BrowseRequest& request) = 0;
  // Distinct non-empty values of `facet` among analyses matching every other
  // filter of `filter`, sorted.
  virtual Result<std::vector<std::string>> facet(BrowseFacet facet, const BrowseFilter& filter) = 0;
  virtual Result<std::optional<AnalysisDetail>> load_analysis_detail(Uuid analysis) = 0;
  // A content-addressed raw series; nullopt when not (yet) uploaded.
  virtual Result<std::optional<BlobData>> load_blob(const Sha256Digest& sha) = 0;
  virtual Result<ChangePage> changes_since(ChangeSeq cursor, int limit) = 0;
  // The newest change_seq (0 for an empty log): a cursor for changes_since
  // that skips the history.
  virtual Result<ChangeSeq> latest_change_seq() = 0;
};

Result<std::unique_ptr<IStore>> open_store(const StoreConfig& config);

// identifier + "-" + two-digit aliquot + step letters (A..Z, AA, ...), as
// legacy make_runid.
std::string make_runid(const std::string& identifier, int aliquot, int increment);

}  // namespace pychron::persistence
