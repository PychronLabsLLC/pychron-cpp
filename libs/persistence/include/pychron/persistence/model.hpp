#pragma once

// Plain value types for the DVC store (DVC schema spec, sections 4-5).
// JSON-valued columns are carried as raw JSON text: persistence never parses
// them, and PostgreSQL may normalise whitespace and key order of jsonb on read.

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/persistence/ids.hpp"

namespace pychron::persistence {

// Revision kinds (section 5.2). One head per (subject, kind).
enum class Kind {
  Signals,
  Intercepts,
  Baselines,
  Blanks,
  IcFactors,
  Tags,
  Annotation,
  RefPins,
  Identity,
  Cosmogenic,
  InterpretedAge,
  RefValue
};

enum class SubjectType { Analysis, Ref, InterpretedAge };

enum class ChangesetKind { Collection, Reduction, Rollback, BookmarkRestore, Import, Reference, Admin };

enum class MoveReason { Commit, Rollback, BookmarkRestore, CollectionRestore, Ingest };

// Database spellings ("icfactors", "bookmark_restore", ...).
std::string_view to_string(Kind kind) noexcept;
std::string_view to_string(SubjectType type) noexcept;
std::string_view to_string(ChangesetKind kind) noexcept;
std::string_view to_string(MoveReason reason) noexcept;
std::optional<Kind> parse_kind(std::string_view text) noexcept;
std::optional<ChangesetKind> parse_changeset_kind(std::string_view text) noexcept;
std::optional<MoveReason> parse_move_reason(std::string_view text) noexcept;

SubjectType subject_type_of(Kind kind) noexcept;

// The kinds every ingested analysis has a root revision for (invariant I3).
inline constexpr Kind kCollectionKinds[] = {Kind::Signals, Kind::Blanks,    Kind::Baselines,
                                            Kind::IcFactors, Kind::Intercepts, Kind::Tags};

// ---------------------------------------------------------------- payload rows

struct ManualOverride {
  bool use_value = false;
  std::optional<double> value;
  bool use_error = false;
  std::optional<double> error;
  friend bool operator==(const ManualOverride&, const ManualOverride&) = default;
};

// `.intercepts` entry, keyed by isotope (free text, so "H1:Ar40" survives).
struct InterceptRow {
  std::string isotope;
  std::string detector;
  std::optional<double> value, error;
  std::optional<std::string> fit, error_type;
  std::optional<int> n, fn;
  std::optional<bool> include_baseline_error;
  std::optional<std::string> filter_outliers_json, user_excluded_json, outlier_excluded_json;
  bool reviewed = false;
  ManualOverride manual;
  std::optional<std::string> extra_json;
  friend bool operator==(const InterceptRow&, const InterceptRow&) = default;
};

// `.baselines` entry, keyed by detector.
struct BaselineRow {
  std::string detector;
  std::optional<double> value, error;
  std::optional<std::string> fit, error_type;
  std::optional<int> n, fn;
  std::optional<std::string> filter_outliers_json, user_excluded_json;
  std::optional<double> modifier_value, modifier_error;
  bool reviewed = false;
  ManualOverride manual;
  std::optional<std::string> extra_json;
  friend bool operator==(const BaselineRow&, const BaselineRow&) = default;
};

// One `references` entry of a blank or IC factor fit.
struct ReferenceRow {
  int ordinal = 0;
  std::optional<Uuid> ref_analysis;
  std::optional<std::string> record_id;
  bool exclude = false;
  friend bool operator==(const ReferenceRow&, const ReferenceRow&) = default;
};

// `.blanks` entry, keyed by isotope.
struct BlankRow {
  std::string isotope;
  std::optional<double> value, error;
  std::optional<std::string> fit, error_type;
  bool reviewed = false;
  ManualOverride manual;
  std::optional<std::string> extra_json;
  std::vector<ReferenceRow> references;
  friend bool operator==(const BlankRow&, const BlankRow&) = default;
};

// `.icfactors` entry, keyed by detector.
struct IcFactorRow {
  std::string detector;
  std::optional<double> value, error;
  std::optional<std::string> fit;
  bool reviewed = false;
  std::optional<std::string> reference_detector;
  std::optional<double> standard_ratio;
  bool discrimination = false;
  bool source_correction = false;
  std::optional<std::string> reference_data_json;
  ManualOverride manual;
  std::optional<std::string> extra_json;
  std::vector<ReferenceRow> references;
  friend bool operator==(const IcFactorRow&, const IcFactorRow&) = default;
};

// `.data` entry: a pointer into an immutable content-addressed blob.
struct SignalRefRow {
  std::string series_kind;  // signal | baseline | sniff | whiff
  std::string series_key;   // isotope; detector for baselines
  std::string detector;
  Sha256Digest blob_sha{};
  std::optional<int> n_points, start_index, end_index;
  friend bool operator==(const SignalRefRow&, const SignalRefRow&) = default;
};

struct TagValue {
  std::string name;
  std::optional<std::string> note;
  std::optional<std::string> subgroup_json;
  friend bool operator==(const TagValue&, const TagValue&) = default;
};

struct AnnotationValue {
  std::optional<std::string> comment;
  friend bool operator==(const AnnotationValue&, const AnnotationValue&) = default;
};

struct RefPinRow {
  Uuid ref_object;
  Uuid ref_revision;
  friend bool operator==(const RefPinRow&, const RefPinRow&) = default;
};

struct CosmogenicValue {
  std::string doc_json;
  friend bool operator==(const CosmogenicValue&, const CosmogenicValue&) = default;
};

using Intercepts = std::vector<InterceptRow>;
using Baselines = std::vector<BaselineRow>;
using Blanks = std::vector<BlankRow>;
using IcFactors = std::vector<IcFactorRow>;
using SignalRefs = std::vector<SignalRefRow>;
using RefPins = std::vector<RefPinRow>;

// An (identifier, aliquot, increment) change (section 5.6). Committing it
// also rewrites the analysis row's identity columns and runid_text in the
// same transaction.
struct IdentityValue {
  Uuid identifier;
  int aliquot = 0;
  int increment = -1;
  std::string reason;  // e.g. "admin_repair", "provisional_renumber"
  friend bool operator==(const IdentityValue&, const IdentityValue&) = default;
};

// ---------------------------------------------------------------- interpreted ages (5.7)

struct InterpretedAgeMember {
  Uuid analysis;
  std::optional<std::string> record_id;
  std::optional<bool> plateau_step;
  std::optional<std::string> tag;
  friend bool operator==(const InterpretedAgeMember&, const InterpretedAgeMember&) = default;
};

struct InterpretedAgeValue {
  std::optional<double> age, age_err;
  std::optional<std::string> age_kind;
  std::optional<double> kca, kca_err, mswd;
  std::optional<int> nanalyses;
  std::string doc_json = "{}";  // the full legacy dict
  std::vector<InterpretedAgeMember> members;
  friend bool operator==(const InterpretedAgeValue&, const InterpretedAgeValue&) = default;
};

// ---------------------------------------------------------------- reference data (6)

enum class RefType {
  FluxPosition,
  LevelGeometry,
  Production,
  LevelProduction,
  Chronology,
  Gains,
  Sensitivity,
  IrradiationHolder,
  LoadHolder,
  Script,
  Document
};

std::string_view to_string(RefType type) noexcept;
std::optional<RefType> parse_ref_type(std::string_view text) noexcept;

struct FluxAnalysis {
  std::optional<Uuid> analysis;
  std::string record_id;
  bool is_omitted = false;
  friend bool operator==(const FluxAnalysis&, const FluxAnalysis&) = default;
};

// flux_position: `<irrad>/<level>.json` positions entry.
struct FluxValue {
  std::optional<double> j, j_err, mean_j, mean_j_err, mean_j_mswd, position_jerr, lambda_k_total, lambda_k_total_err;
  std::optional<std::string> monitor_name, monitor_material;
  std::optional<double> monitor_age, monitor_age_err;
  std::optional<std::string> options_json, extra_json;
  std::vector<FluxAnalysis> analyses;  // keyed by record_id
  friend bool operator==(const FluxValue&, const FluxValue&) = default;
};

// level_geometry
struct LevelZValue {
  std::optional<double> z;
  friend bool operator==(const LevelZValue&, const LevelZValue&) = default;
};

struct ProductionRatio {
  std::string key;  // INTERFERENCE_KEYS + RATIO_KEYS
  double value = 0;
  double error = 0;
  friend bool operator==(const ProductionRatio&, const ProductionRatio&) = default;
};

// production
struct ProductionValue {
  std::optional<std::string> reactor, note;
  std::vector<ProductionRatio> ratios;  // keyed by key
  friend bool operator==(const ProductionValue&, const ProductionValue&) = default;
};

// level_production: which production a level uses.
struct LevelProductionValue {
  Uuid production;  // a ref_object of type production
  std::optional<std::string> note;
  friend bool operator==(const LevelProductionValue&, const LevelProductionValue&) = default;
};

struct Dose {
  int ordinal = 0;
  double power = 0;
  UtcTime start, end;
  friend bool operator==(const Dose&, const Dose&) = default;
};

// chronology
struct ChronologyValue {
  std::vector<Dose> doses;  // keyed by ordinal
  friend bool operator==(const ChronologyValue&, const ChronologyValue&) = default;
};

struct DetectorGain {
  std::string detector;
  double gain = 1;
  friend bool operator==(const DetectorGain&, const DetectorGain&) = default;
};

// gains
struct GainsValue {
  std::vector<DetectorGain> gains;  // keyed by detector
  friend bool operator==(const GainsValue&, const GainsValue&) = default;
};

// sensitivity: one revision per legacy record.
struct SensitivityValue {
  double sensitivity = 0;
  std::optional<UtcTime> create_date;
  std::optional<std::string> extra_json;
  friend bool operator==(const SensitivityValue&, const SensitivityValue&) = default;
};

struct HolderHole {
  int ordinal = 0;
  std::string hole_id;
  double x = 0, y = 0;
  std::optional<double> radius;
  friend bool operator==(const HolderHole&, const HolderHole&) = default;
};

// irradiation_holder, load_holder
struct HolderValue {
  std::optional<std::string> shape;
  std::optional<double> radius;
  bool has_hole_numbers = false;
  std::vector<HolderHole> holes;  // keyed by ordinal
  friend bool operator==(const HolderValue&, const HolderValue&) = default;
};

// script: the text is stored once in script_text, keyed by SHA-256 of the body.
struct ScriptValue {
  std::string body;
  friend bool operator==(const ScriptValue&, const ScriptValue&) = default;
};

// document: an opaque meta-repo file (reactors.json, ...).
struct DocumentValue {
  std::optional<std::string> content_text, content_json;
  friend bool operator==(const DocumentValue&, const DocumentValue&) = default;
};

// The payload of a `value` revision of a ref_object. The alternative must
// match the object's ref_type (ref_payload_matches); that is checked at commit.
using RefPayload = std::variant<FluxValue, LevelZValue, ProductionValue, LevelProductionValue, ChronologyValue,
                                GainsValue, SensitivityValue, HolderValue, ScriptValue, DocumentValue>;

bool ref_payload_matches(RefType type, const RefPayload& payload) noexcept;

// The payload of one revision. The alternative must match the revision kind
// (payload_kind_matches).
using RevisionPayload = std::variant<Intercepts, Baselines, Blanks, IcFactors, SignalRefs, TagValue, AnnotationValue,
                                     RefPins, CosmogenicValue, IdentityValue, InterpretedAgeValue, RefPayload>;

bool payload_kind_matches(Kind kind, const RevisionPayload& payload) noexcept;

// ---------------------------------------------------------------- audit records

struct ChangesetInfo {
  Uuid uuid;
  ChangesetKind kind = ChangesetKind::Reduction;
  Uuid author_user;
  Uuid client;
  UtcTime created;
  std::string message;
  friend bool operator==(const ChangesetInfo&, const ChangesetInfo&) = default;
};

struct RevisionInfo {
  Uuid uuid;
  Uuid subject;
  Kind kind = Kind::Intercepts;
  std::optional<Uuid> parent;
  ChangesetInfo changeset;
  ChangeSeq change_seq = 0;  // when the revision became visible
  std::string author_name;      // app_user.name of the changeset author
  std::string client_hostname;  // client.hostname of the changeset
};

struct HeadInfo {
  Uuid subject;
  Kind kind = Kind::Intercepts;
  Uuid revision;
  int head_version = 1;
};

}  // namespace pychron::persistence
