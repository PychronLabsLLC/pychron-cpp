// Accounting: every unit of the source is imported or explained
// (verify_parts.hpp, legacy ingestion spec sections 6 and 10.8).

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "pychron/ingest/ids.hpp"
#include "verify_parts.hpp"

namespace pychron::ingest::detail {

namespace P = pychron::persistence;
using Json = nlohmann::json;

namespace {

bool needs_evidence(UnitDisposition disposition) {
  switch (disposition) {
    case UnitDisposition::Imported:
    case UnitDisposition::Folded:
    case UnitDisposition::Unchanged:
    case UnitDisposition::Conflict:
      return true;
    case UnitDisposition::Ignored:
    case UnitDisposition::Removed:
    case UnitDisposition::Unclassified:
      break;
  }
  return false;
}

}  // namespace

Result<void> Accountant::check(const SourceUnit& unit, VerifyReport& report) {
  ++report.units;
  if (unit.disposition == UnitDisposition::Ignored) {
    ++report.ignored;
    return {};
  }
  UnaccountedUnit open{unit, {}};
  if (unit.disposition != UnitDisposition::Unclassified) {
    for (const auto& evidence : unit.evidence) {
      auto there = found(evidence);
      if (!there) return fail(there.error());
      if (!*there) open.missing.push_back(evidence);
    }
    // A unit that names nothing is accounted for only as a plain deletion.
    if (open.missing.empty() && !(unit.evidence.empty() && needs_evidence(unit.disposition))) return {};
  }
  report.unaccounted.push_back(std::move(open));
  return {};
}

Result<bool> Accountant::conflict_stored(const std::string& commit, const std::string& path) {
  auto row = source_.store.import_conflict(conflict_id(source_.url, commit, path));
  if (!row) return fail(row.error());
  return row->has_value();
}

Result<bool> Accountant::found(const Evidence& evidence) {
  P::IStore& store = source_.store;
  switch (evidence.kind) {
    case Evidence::Kind::Recorded: {
      auto recorded = store.has_provenance(source_.uuid, evidence.commit, evidence.path);
      if (!recorded || *recorded) return recorded;
      auto refused = conflict_stored(evidence.commit, evidence.path);
      if (!refused || *refused || evidence.blob_sha.empty()) return refused;
      return store.has_provenance_blob(source_.uuid, evidence.path, evidence.blob_sha);
    }
    case Evidence::Kind::Conflict:
      return conflict_stored(evidence.commit, evidence.path);
    case Evidence::Kind::Blob:
      if (evidence.blob_sha.empty()) return false;
      return store.has_provenance_blob(source_.uuid, evidence.path, evidence.blob_sha);
    case Evidence::Kind::Entity: {
      auto rows = store.provenance_for(evidence.entity);
      if (!rows) return fail(rows.error());
      for (const auto& row : *rows)
        if (row.source == source_.uuid) return true;
      return false;
    }
    case Evidence::Kind::Note:
      return noted(evidence);
    case Evidence::Kind::CatalogRow:
      if (!evidence.catalog) return false;
      return catalog_row_exists(store, *evidence.catalog);
  }
  return false;
}

// Whether the provenance detail of the commit's changeset lists the path:
// {"<list>": ["<path>", {"path": "<path>", ...}, ...]}.
Result<bool> Accountant::noted(const Evidence& evidence) {
  if (!noted_commit_ || *noted_commit_ != evidence.commit) {
    notes_.clear();
    noted_commit_ = evidence.commit;
    auto rows = source_.store.provenance_for(changeset_id(source_.url, evidence.commit));
    if (!rows) return fail(rows.error());
    try {
      for (const auto& row : *rows) {
        if (row.entity_type != "changeset" || row.source != source_.uuid || !row.detail_json) continue;
        const Json detail = Json::parse(*row.detail_json, nullptr, false);
        if (!detail.is_object()) continue;
        for (const auto& [name, list] : detail.items()) {
          if (!list.is_array()) continue;
          auto& paths = notes_[name];
          for (const auto& entry : list) {
            if (entry.is_string()) {
              paths.insert(entry.get<std::string>());
            } else if (entry.is_object()) {
              const auto path = entry.find("path");
              if (path != entry.end() && path->is_string()) paths.insert(path->get<std::string>());
            }
          }
        }
      }
    } catch (const std::exception& e) {
      return fail(ErrorKind::Protocol, "verify: provenance detail of commit " + evidence.commit + ": " + e.what());
    }
  }
  const auto list = notes_.find(evidence.list);
  return list != notes_.end() && list->second.contains(evidence.path);
}

}  // namespace pychron::ingest::detail
