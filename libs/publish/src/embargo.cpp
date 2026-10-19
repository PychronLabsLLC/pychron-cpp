// Embargoes by name (data publishing design, section 4.2).

#include "pychron/publish/embargo.hpp"

#include <algorithm>
#include <cctype>

namespace pychron::publish {

namespace ps = persistence;

std::optional<RunId> parse_runid(std::string_view text) {
  const auto dash = text.rfind('-');
  if (dash == std::string_view::npos || dash == 0 || dash + 1 >= text.size()) return std::nullopt;
  RunId out;
  out.identifier = std::string(text.substr(0, dash));
  std::string_view rest = text.substr(dash + 1);
  std::size_t i = 0;
  int aliquot = 0;
  while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i]))) {
    aliquot = aliquot * 10 + (rest[i] - '0');
    ++i;
  }
  if (i == 0) return std::nullopt;
  out.aliquot = aliquot;
  if (i == rest.size()) return out;
  // Step letters: A..Z, AA.., bijective base 26, as make_runid writes them.
  int increment = 0;
  for (; i < rest.size(); ++i) {
    const char c = rest[i];
    if (c < 'A' || c > 'Z') return std::nullopt;
    increment = increment * 26 + (c - 'A' + 1);
  }
  out.increment = increment - 1;
  return out;
}

namespace {

Result<ps::ProjectRow> project_named(ps::IStore& store, std::string_view name) {
  auto rows = store.projects(std::nullopt);
  if (!rows) return fail(rows.error());
  std::vector<ps::ProjectRow> matches;
  for (const auto& p : *rows)
    if (p.name == name) matches.push_back(p);
  if (matches.empty()) return fail(ErrorKind::Config, "no project named '" + std::string(name) + "'");
  if (matches.size() > 1) {
    std::string who;
    for (const auto& m : matches) who += (who.empty() ? "" : ", ") + (m.principal_investigator_name.empty() ? "no PI" : m.principal_investigator_name);
    return fail(ErrorKind::Config, "project '" + std::string(name) + "' belongs to several PIs (" + who + "); give its uuid");
  }
  return matches.front();
}

Result<ps::Uuid> project_scope(ps::IStore& store, std::string_view text) {
  if (auto uuid = ps::Uuid::parse(text)) return *uuid;
  auto row = project_named(store, text);
  if (!row) return fail(row.error());
  return row->uuid;
}

Result<ps::Uuid> sample_scope(ps::IStore& store, std::string_view text) {
  if (auto uuid = ps::Uuid::parse(text)) return *uuid;
  std::string name(text);
  std::optional<ps::Uuid> project;
  if (const auto at = name.rfind('@'); at != std::string::npos) {
    auto p = project_named(store, name.substr(at + 1));
    if (!p) return fail(p.error());
    project = p->uuid;
    name.resize(at);
  }
  ps::SampleQuery q;
  q.text = name;  // a search, narrowed below to the exact name: the lookup is by the name, once
  q.project = project;
  q.counts = false;
  q.limit = 1000;
  auto rows = store.samples(q);
  if (!rows) return fail(rows.error());
  std::vector<ps::SampleRow> matches;
  for (const auto& s : *rows)
    if (s.name == name) matches.push_back(s);
  if (matches.empty()) return fail(ErrorKind::Config, "no sample named '" + name + "'" + (project ? " in that project" : ""));
  if (matches.size() > 1) {
    std::string where;
    for (const auto& m : matches) where += (where.empty() ? "" : ", ") + m.project_name;
    return fail(ErrorKind::Config, "sample '" + name + "' is in several projects (" + where + "); say '" + name +
                                       "@<project>' or give its uuid");
  }
  return matches.front().uuid;
}

Result<ps::Uuid> analysis_scope(ps::IStore& store, std::string_view text) {
  if (auto uuid = ps::Uuid::parse(text)) return *uuid;
  const auto runid = parse_runid(text);
  if (!runid) return fail(ErrorKind::Config, "'" + std::string(text) + "' is neither a run id nor an analysis uuid");
  auto found = store.find_analysis(runid->identifier, runid->aliquot, runid->increment);
  if (!found) return fail(found.error());
  if (!*found) return fail(ErrorKind::Config, "no analysis " + std::string(text));
  return **found;
}

Result<std::string> scope_name(ps::IStore& store, const ps::EmbargoScope& scope) {
  switch (scope.type) {
    case ps::EmbargoScopeType::Project: {
      auto rows = store.projects(std::nullopt);
      if (!rows) return fail(rows.error());
      for (const auto& p : *rows)
        if (p.uuid == scope.uuid) return "project " + p.name;
      return "project " + scope.uuid.str();
    }
    case ps::EmbargoScopeType::Sample: {
      ps::SampleQuery q;
      q.uuid = scope.uuid;
      q.counts = false;
      q.limit = 1;
      auto rows = store.samples(q);
      if (!rows) return fail(rows.error());
      if (rows->empty()) return "sample " + scope.uuid.str();
      return "sample " + rows->front().name + " (" + rows->front().project_name + ")";
    }
    case ps::EmbargoScopeType::Analysis: {
      auto detail = store.load_analysis_detail(scope.uuid);
      if (!detail) return fail(detail.error());
      if (!*detail) return "analysis " + scope.uuid.str();
      return "analysis " + (*detail)->row.summary.runid;
    }
  }
  return scope.uuid.str();
}

}  // namespace

Result<ps::EmbargoScope> resolve_scope(ps::IStore& store, ps::EmbargoScopeType type, std::string_view name_or_uuid) {
  if (name_or_uuid.empty()) return fail(ErrorKind::Config, "an embargo needs a project, sample or analysis");
  Result<ps::Uuid> uuid = fail(ErrorKind::Config, "unknown scope");
  switch (type) {
    case ps::EmbargoScopeType::Project:
      uuid = project_scope(store, name_or_uuid);
      break;
    case ps::EmbargoScopeType::Sample:
      uuid = sample_scope(store, name_or_uuid);
      break;
    case ps::EmbargoScopeType::Analysis:
      uuid = analysis_scope(store, name_or_uuid);
      break;
  }
  if (!uuid) return fail(uuid.error());
  return ps::EmbargoScope{type, *uuid};
}

Result<Uuid> set_embargo(ps::IStore& store, const ps::Actor& actor, const EmbargoRequest& request) {
  auto scope = resolve_scope(store, request.type, request.name_or_uuid);
  if (!scope) return fail(scope.error());
  return store.add_embargo(actor, ps::EmbargoInsert{*scope, request.until, request.reason});
}

Result<void> lift_embargo(ps::IStore& store, const ps::Actor& actor, std::string_view embargo_uuid) {
  auto uuid = Uuid::parse(embargo_uuid);
  if (!uuid) return fail(ErrorKind::Config, "'" + std::string(embargo_uuid) + "' is no embargo uuid");
  return store.lift_embargo(actor, *uuid);
}

Result<std::size_t> lift_embargoes_on(ps::IStore& store, const ps::Actor& actor, ps::EmbargoScopeType type,
                                      std::string_view name_or_uuid) {
  auto scope = resolve_scope(store, type, name_or_uuid);
  if (!scope) return fail(scope.error());
  auto live = store.embargoes(true, UtcTime::now());
  if (!live) return fail(live.error());
  std::size_t n = 0;
  for (const auto& e : *live) {
    if (e.scope != *scope) continue;
    if (auto r = store.lift_embargo(actor, e.uuid); !r) return fail(r.error());
    ++n;
  }
  return n;
}

Result<std::vector<EmbargoListing>> list_embargoes(ps::IStore& store, bool live_only, UtcTime at) {
  auto rows = store.embargoes(live_only, at);
  if (!rows) return fail(rows.error());
  std::vector<EmbargoListing> out;
  out.reserve(rows->size());
  for (const auto& r : *rows) {
    auto name = scope_name(store, r.scope);
    if (!name) return fail(name.error());
    out.push_back({r, std::move(*name)});
  }
  return out;
}

}  // namespace pychron::publish
