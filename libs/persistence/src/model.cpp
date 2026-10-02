#include "pychron/persistence/model.hpp"

#include <array>
#include <utility>

namespace pychron::persistence {
namespace {

constexpr std::array<std::pair<Kind, std::string_view>, 12> kKinds = {{
    {Kind::Signals, "signals"},
    {Kind::Intercepts, "intercepts"},
    {Kind::Baselines, "baselines"},
    {Kind::Blanks, "blanks"},
    {Kind::IcFactors, "icfactors"},
    {Kind::Tags, "tags"},
    {Kind::Annotation, "annotation"},
    {Kind::RefPins, "refpins"},
    {Kind::Identity, "identity"},
    {Kind::Cosmogenic, "cosmogenic"},
    {Kind::InterpretedAge, "interpreted_age"},
    {Kind::RefValue, "value"},
}};

constexpr std::array<std::pair<ChangesetKind, std::string_view>, 7> kChangesetKinds = {{
    {ChangesetKind::Collection, "collection"},
    {ChangesetKind::Reduction, "reduction"},
    {ChangesetKind::Rollback, "rollback"},
    {ChangesetKind::BookmarkRestore, "bookmark_restore"},
    {ChangesetKind::Import, "import"},
    {ChangesetKind::Reference, "reference"},
    {ChangesetKind::Admin, "admin"},
}};

constexpr std::array<std::pair<MoveReason, std::string_view>, 5> kMoveReasons = {{
    {MoveReason::Commit, "commit"},
    {MoveReason::Rollback, "rollback"},
    {MoveReason::BookmarkRestore, "bookmark_restore"},
    {MoveReason::CollectionRestore, "collection_restore"},
    {MoveReason::Ingest, "ingest"},
}};

template <class E, std::size_t N>
std::string_view name_of(const std::array<std::pair<E, std::string_view>, N>& table, E value) noexcept {
  for (const auto& [e, name] : table)
    if (e == value) return name;
  return {};
}

template <class E, std::size_t N>
std::optional<E> value_of(const std::array<std::pair<E, std::string_view>, N>& table, std::string_view text) noexcept {
  for (const auto& [e, name] : table)
    if (name == text) return e;
  return std::nullopt;
}

}  // namespace

std::string_view to_string(Kind kind) noexcept { return name_of(kKinds, kind); }
std::string_view to_string(ChangesetKind kind) noexcept { return name_of(kChangesetKinds, kind); }
std::string_view to_string(MoveReason reason) noexcept { return name_of(kMoveReasons, reason); }

std::string_view to_string(SubjectType type) noexcept {
  switch (type) {
    case SubjectType::Analysis:
      return "analysis";
    case SubjectType::Ref:
      return "ref";
    case SubjectType::InterpretedAge:
      return "ia";
  }
  return {};
}

std::optional<Kind> parse_kind(std::string_view text) noexcept { return value_of(kKinds, text); }
std::optional<ChangesetKind> parse_changeset_kind(std::string_view text) noexcept {
  return value_of(kChangesetKinds, text);
}
std::optional<MoveReason> parse_move_reason(std::string_view text) noexcept { return value_of(kMoveReasons, text); }

SubjectType subject_type_of(Kind kind) noexcept {
  switch (kind) {
    case Kind::RefValue:
      return SubjectType::Ref;
    case Kind::InterpretedAge:
      return SubjectType::InterpretedAge;
    default:
      return SubjectType::Analysis;
  }
}

bool payload_kind_matches(Kind kind, const RevisionPayload& payload) noexcept {
  switch (kind) {
    case Kind::Intercepts:
      return std::holds_alternative<Intercepts>(payload);
    case Kind::Baselines:
      return std::holds_alternative<Baselines>(payload);
    case Kind::Blanks:
      return std::holds_alternative<Blanks>(payload);
    case Kind::IcFactors:
      return std::holds_alternative<IcFactors>(payload);
    case Kind::Signals:
      return std::holds_alternative<SignalRefs>(payload);
    case Kind::Tags:
      return std::holds_alternative<TagValue>(payload);
    case Kind::Annotation:
      return std::holds_alternative<AnnotationValue>(payload);
    case Kind::RefPins:
      return std::holds_alternative<RefPins>(payload);
    case Kind::Cosmogenic:
      return std::holds_alternative<CosmogenicValue>(payload);
    case Kind::Identity:
      return std::holds_alternative<IdentityValue>(payload);
    case Kind::InterpretedAge:
      return std::holds_alternative<InterpretedAgeValue>(payload);
    case Kind::RefValue:
      return std::holds_alternative<RefPayload>(payload);
  }
  return false;
}

namespace {
constexpr std::array<std::pair<RefType, std::string_view>, 11> kRefTypes = {{
    {RefType::FluxPosition, "flux_position"},
    {RefType::LevelGeometry, "level_geometry"},
    {RefType::Production, "production"},
    {RefType::LevelProduction, "level_production"},
    {RefType::Chronology, "chronology"},
    {RefType::Gains, "gains"},
    {RefType::Sensitivity, "sensitivity"},
    {RefType::IrradiationHolder, "irradiation_holder"},
    {RefType::LoadHolder, "load_holder"},
    {RefType::Script, "script"},
    {RefType::Document, "document"},
}};
}  // namespace

std::string_view to_string(RefType type) noexcept { return name_of(kRefTypes, type); }
std::optional<RefType> parse_ref_type(std::string_view text) noexcept { return value_of(kRefTypes, text); }

bool ref_payload_matches(RefType type, const RefPayload& payload) noexcept {
  switch (type) {
    case RefType::FluxPosition:
      return std::holds_alternative<FluxValue>(payload);
    case RefType::LevelGeometry:
      return std::holds_alternative<LevelZValue>(payload);
    case RefType::Production:
      return std::holds_alternative<ProductionValue>(payload);
    case RefType::LevelProduction:
      return std::holds_alternative<LevelProductionValue>(payload);
    case RefType::Chronology:
      return std::holds_alternative<ChronologyValue>(payload);
    case RefType::Gains:
      return std::holds_alternative<GainsValue>(payload);
    case RefType::Sensitivity:
      return std::holds_alternative<SensitivityValue>(payload);
    case RefType::IrradiationHolder:
    case RefType::LoadHolder:
      return std::holds_alternative<HolderValue>(payload);
    case RefType::Script:
      return std::holds_alternative<ScriptValue>(payload);
    case RefType::Document:
      return std::holds_alternative<DocumentValue>(payload);
  }
  return false;
}

}  // namespace pychron::persistence
