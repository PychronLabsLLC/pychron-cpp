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
#include "pychron/persistence/catalog.hpp"
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
  std::optional<std::string> kind = std::nullopt;
  std::optional<std::string> code = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;  // used when the row is created; ignored when it exists
};

// A user (table `app_user`); `name` is its natural key.
struct UserSpec {
  std::string name;
  std::optional<std::string> email = std::nullopt, affiliation = std::nullopt, category = std::nullopt;
};

// A package (E13 of the entry spec): table `irradiation`.
struct IrradiationSpec {
  std::string name;
  std::optional<UtcTime> created = std::nullopt;  // created_utc; the write time when unset
  std::optional<std::string> kind = std::nullopt;  // irradiation (default) | package
};

struct IdentifierSpec {
  std::string identifier;
  std::string kind = "unknown";               // unknown | special
  std::optional<std::string> analysis_type = std::nullopt;  // required for special identifiers
  std::optional<Uuid> mass_spectrometer = std::nullopt;
  std::optional<Uuid> position = std::nullopt;  // irradiation_position (unknowns only)
  std::optional<Uuid> sample = std::nullopt;  // when there is no irradiation position
  std::optional<Uuid> uuid = std::nullopt;
};

struct PrincipalInvestigatorSpec {
  std::string last_name;
  std::string first_initial{};
  std::optional<std::string> affiliation = std::nullopt, email = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;
};

struct ProjectSpec {
  std::string name;
  std::optional<Uuid> principal_investigator = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;
  // A calendar date, "YYYY-MM-DD"; anything else is an error.
  std::optional<std::string> checkin_date = std::nullopt;
  std::optional<std::string> comment = std::nullopt, lab_contact = std::nullopt, institution = std::nullopt;
};

struct MaterialSpec {
  std::string name;
  std::string grainsize{};
  std::optional<Uuid> uuid = std::nullopt;
};

struct SampleSpec {
  std::string name;
  Uuid project;
  Uuid material;
  std::optional<std::string> note = std::nullopt, igsn = std::nullopt;
  std::optional<double> lat = std::nullopt, lon = std::nullopt;
  std::optional<double> elevation = std::nullopt;
  std::optional<std::string> storage_location = std::nullopt, location = std::nullopt, unit = std::nullopt;
  std::optional<std::string> lithology = std::nullopt, lithology_class = std::nullopt;
  std::optional<std::string> lithology_type = std::nullopt, lithology_group = std::nullopt;
  std::optional<double> approximate_age = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;
  std::optional<UtcTime> created = std::nullopt;  // created_utc; the write time when unset
  std::optional<UtcTime> updated = std::nullopt;  // updated_utc; `created`, else the write time, when unset
};

struct LevelSpec {
  Uuid irradiation;
  std::string name;
  std::optional<Uuid> holder = std::nullopt;  // ref_object of type irradiation_holder
  std::optional<double> z = std::nullopt;
  std::optional<std::string> note = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;
};

struct PositionSpec {
  Uuid level;
  int position = 0;
  std::optional<Uuid> sample = std::nullopt;
  std::optional<double> weight = std::nullopt;
  std::optional<std::string> packet = std::nullopt, note = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;
};

// A reference object (section 6.1). `key` is unique per ref_type; the scope
// columns are what resolve_refs() matches an analysis against.
struct RefObjectSpec {
  RefType type = RefType::Document;
  std::string key;  // "<irrad>/<level>/<pos>", "<irrad>", "<ms>", ...
  std::optional<Uuid> irradiation = std::nullopt, level = std::nullopt;
  std::optional<Uuid> position = std::nullopt, mass_spectrometer = std::nullopt;
  std::optional<Uuid> uuid = std::nullopt;
};

// A sample load (table `load`); `name` is its natural key.
struct LoadSpec {
  std::string name;
  std::optional<Uuid> holder = std::nullopt;  // ref_object of type load_holder
  std::optional<Uuid> holder_revision = std::nullopt;  // the holder's value revision the load was made against
  std::optional<Uuid> created_by_user = std::nullopt;
  bool archived = false;
  std::optional<UtcTime> created = std::nullopt;  // created_utc; the write time when unset
  std::optional<Uuid> uuid = std::nullopt;
};

// One tray position of a load (table `load_position`); (load, position,
// identifier) is its natural key.
struct LoadPositionSpec {
  Uuid load;
  int position = 0;
  Uuid identifier;
  std::optional<double> weight = std::nullopt;
  std::optional<int> nxtals = std::nullopt;
  std::optional<std::string> note = std::nullopt;
};

struct InterpretedAgeSpec {
  std::string name;
  std::optional<Uuid> identifier = std::nullopt;
  std::optional<Uuid> repository = std::nullopt;
  // When set, the interpreted age is ensured by this id: an existing one with
  // it is returned and nothing is written. Without it every call adds a row
  // (the table has no natural key).
  std::optional<Uuid> uuid = std::nullopt;
};

// Bookmarks capture the heads of every analysis in a repository or a group
// (section 5.5). Exactly one scope must be set.
struct BookmarkSpec {
  std::string name;
  std::optional<std::string> message = std::nullopt;
  std::optional<Uuid> repository = std::nullopt;
  std::optional<Uuid> group = std::nullopt;
  // When set, the bookmark is ensured by this id: an existing bookmark with
  // it is returned and nothing is written.
  std::optional<Uuid> uuid = std::nullopt;
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
  virtual Result<std::optional<ImportConflictRow>> import_conflict(Uuid conflict) = 0;
  virtual Result<std::vector<ProvenanceRow>> provenance_for(Uuid entity) = 0;
  virtual Result<bool> has_provenance(Uuid source, std::string_view commit_sha, std::string_view path) = 0;
  virtual Result<bool> has_provenance_blob(Uuid source, std::string_view path, std::string_view git_blob_sha) = 0;
  virtual Result<bool> has_conflict(Uuid source, std::string_view path, const Sha256Digest& file_sha256) = 0;
  // The git blob sha recorded for the current head revision of (subject, kind)
  // from this source; nullopt if none.
  virtual Result<std::optional<std::string>> imported_head_blob_sha(Uuid source, Uuid subject, Kind kind) = 0;

  // Catalog (not revisioned; every write is in change_entity with a field diff, D6).
  // Every add_* is ensure-by-natural-key (the table's UNIQUE columns): when the
  // row exists its uuid is returned and every value it has is kept, whatever
  // the spec says. What the row lacks (a NULL column that is not part of the
  // key) and the spec has is filled, as one audited update; a fill that breaks
  // a constraint fails as the insert would, and writes nothing. `uuid` in a
  // spec is used only when the row is created.
  virtual Result<Uuid> register_client(const ClientRegistration& registration) = 0;
  virtual Result<Uuid> ensure_user(Uuid client, const std::string& name) = 0;
  // As ensure_user, with the descriptive columns.
  virtual Result<Uuid> add_user(Uuid client, const UserSpec& spec) = 0;
  virtual Result<Uuid> add_mass_spectrometer(Uuid client, const MassSpectrometerSpec& spec) = 0;
  virtual Result<Uuid> add_identifier(Uuid client, const IdentifierSpec& spec) = 0;
  virtual Result<Uuid> add_extract_device(Uuid client, const std::string& name) = 0;
  virtual Result<Uuid> add_principal_investigator(Uuid client, const PrincipalInvestigatorSpec& spec) = 0;
  virtual Result<Uuid> add_project(Uuid client, const ProjectSpec& spec) = 0;
  virtual Result<Uuid> add_material(Uuid client, const MaterialSpec& spec) = 0;
  virtual Result<Uuid> add_sample(Uuid client, const SampleSpec& spec) = 0;
  virtual Result<Uuid> add_irradiation(Uuid client, const std::string& name) = 0;
  virtual Result<Uuid> add_irradiation(Uuid client, const IrradiationSpec& spec) = 0;
  virtual Result<Uuid> add_level(Uuid client, const LevelSpec& spec) = 0;
  virtual Result<Uuid> add_irradiation_position(Uuid client, const PositionSpec& spec) = 0;
  // A reference object; its values are `value` revisions staged through a unit
  // of work with Kind::RefValue and a RefPayload matching its type.
  virtual Result<Uuid> add_ref_object(Uuid client, const RefObjectSpec& spec) = 0;
  virtual Result<Uuid> add_load(Uuid client, const LoadSpec& spec) = 0;
  virtual Result<void> add_load_position(Uuid client, const LoadPositionSpec& spec) = 0;
  // An interpreted age; its values are Kind::InterpretedAge revisions.
  virtual Result<Uuid> add_interpreted_age(Uuid client, const InterpretedAgeSpec& spec) = 0;

  // Lookups by natural key. Unlike add_*, they create nothing: an importer
  // asks them before it sends something that would collide.
  virtual Result<std::optional<Uuid>> find_identifier(const std::string& identifier) = 0;
  // The analysis that has this run identity now (increment -1: no step).
  virtual Result<std::optional<Uuid>> find_analysis(const std::string& identifier, int aliquot, int increment) = 0;
  // The identifier that sits at an irradiation position; nullopt when the
  // position does not exist or holds none.
  virtual Result<std::optional<std::string>> identifier_at(const std::string& irradiation, const std::string& level,
                                                           int position) = 0;
  // The row of `table` that `key` names (see CatalogTable for the parts);
  // nullopt when there is none. A key with the wrong number of parts is an
  // error. This is the lookup every add_* makes before it writes.
  virtual Result<std::optional<Uuid>> find_catalog_row(CatalogTable table, const std::vector<CatalogKeyPart>& key) = 0;

  // Entry reads (entry spec 5.1). Packages are listed newest first, levels
  // by name, positions by position.
  virtual Result<std::vector<PrincipalInvestigatorRow>> principal_investigators() = 0;
  virtual Result<std::vector<ProjectRow>> projects(std::optional<Uuid> principal_investigator) = 0;
  virtual Result<std::vector<MaterialRow>> materials() = 0;
  virtual Result<std::vector<SampleRow>> samples(const SampleQuery& query) = 0;
  virtual Result<std::vector<IrradiationRow>> irradiations() = 0;
  virtual Result<std::vector<LevelRow>> levels(Uuid irradiation) = 0;
  virtual Result<std::optional<LevelSheet>> level_sheet(Uuid level) = 0;
  // The current value of identifier_counter's `scope`; nullopt before the first allocation.
  virtual Result<std::optional<std::int64_t>> identifier_counter(const std::string& scope) = 0;
  // The largest identifier that is all ASCII digits (no leading zero, at most
  // 18 of them); 0 when there is none. What the counter is seeded from.
  virtual Result<std::int64_t> max_numeric_identifier() = 0;
  // The current values of one catalog row's columns; nullopt when it is gone.
  virtual Result<std::optional<CatalogFields>> catalog_row(CatalogTable table, Uuid uuid) = 0;

  // Entry writes (entry spec 5.2-5.4). One transaction each: a stale row, a
  // refusal or a lost reference CAS writes nothing and reports every one found.
  virtual Result<CatalogOutcome> apply_catalog_edits(Uuid client, const CatalogEditBatch& batch) = 0;
  // The batch and the revisions staged in `refs` (a unit of work from begin()
  // of this store), committed together as one `kind` changeset. `refs` is
  // consumed as by commit().
  virtual Result<CatalogOutcome> apply_catalog_edits(const Actor& actor, const CatalogEditBatch& batch,
                                                     IUnitOfWork& refs, ChangesetKind kind,
                                                     std::string message) = 0;
  // Sequential identifiers from identifier_counter (scope kIdentifierScope).
  virtual Result<AllocationOutcome> allocate_identifiers(Uuid client, const IdentifierAllocation& allocation) = 0;

  // Groups, repositories, bookmarks (sections 3.6, 5.5).
  virtual Result<Uuid> add_repository(Uuid client, const std::string& name) = 0;
  virtual Result<void> add_repository_members(const Actor& actor, Uuid repository,
                                              const std::vector<Uuid>& analyses) = 0;
  // With `uuid`, the group is ensured by that id: an existing group with it
  // is returned and nothing is written, members included.
  virtual Result<Uuid> create_group(const Actor& actor, const std::string& name, const std::vector<Uuid>& analyses,
                                    std::optional<Uuid> uuid = std::nullopt) = 0;
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
  virtual Result<bool> has_revision(Uuid revision) = 0;
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

// True for the error IStore::ingest returns when an analysis names an
// identifier, mass spectrometer or extract device that is not in the catalog.
// Nothing was written; the item can be ingested once the row exists.
bool is_unknown_catalog_reference(const Error& error) noexcept;

// True for the error an add_* catalog call returns when the row it names
// exists and an integrity constraint keeps out the values the call would
// fill it with (a position another identifier holds, a position for a special
// identifier, a spectrometer code another spectrometer has). It is the
// database's own code that decides: SQLSTATE class 23 on PostgreSQL, the
// primary result code SQLITE_CONSTRAINT (19, with its extended codes, which
// include a trigger's RAISE) on SQLite. Nothing was written and the row is as
// it was. The error is known by Error::code, not by its words, so it stays
// this error when a caller adds context to `what`. Not this error: a new row
// that is refused, and an update that fails for any other reason (no
// permission, a missing column, a PostgreSQL trigger that raises, bad SQL, a
// lost connection, a busy database).
bool is_refused_catalog_fill(const Error& error) noexcept;

// identifier + "-" + two-digit aliquot + step letters (A..Z, AA, ...), as
// legacy make_runid.
std::string make_runid(const std::string& identifier, int aliquot, int increment);

}  // namespace pychron::persistence
