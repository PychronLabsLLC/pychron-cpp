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

// The payload of one revision. The alternative must match the revision kind
// (payload_kind_matches). Identity, interpreted-age and reference payloads are
// not modelled yet; revisions of those kinds cannot be staged.
using RevisionPayload =
    std::variant<Intercepts, Baselines, Blanks, IcFactors, SignalRefs, TagValue, AnnotationValue, RefPins, CosmogenicValue>;

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
};

struct HeadInfo {
  Uuid subject;
  Kind kind = Kind::Intercepts;
  Uuid revision;
  int head_version = 1;
};

}  // namespace pychron::persistence
