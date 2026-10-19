// Creating, publishing and withdrawing a release (data publishing design,
// sections 4.3 and 4.4).

#include "pychron/publish/release.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace pychron::publish {

namespace ps = persistence;

std::string authors_json(const std::vector<Author>& authors) {
  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  for (const auto& a : authors) {
    nlohmann::ordered_json j;
    j["name"] = a.name;
    if (a.orcid) j["orcid"] = *a.orcid;
    if (a.affiliation) j["affiliation"] = *a.affiliation;
    out.push_back(std::move(j));
  }
  return out.dump();
}

Result<std::vector<Author>> parse_authors(std::string_view json) {
  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
  if (!parsed.is_array()) return fail(ErrorKind::Config, "authors are not a JSON list");
  std::vector<Author> out;
  for (const auto& j : parsed) {
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string())
      return fail(ErrorKind::Config, "an author has no name");
    Author a;
    a.name = j["name"].get<std::string>();
    if (j.contains("orcid") && j["orcid"].is_string()) a.orcid = j["orcid"].get<std::string>();
    if (j.contains("affiliation") && j["affiliation"].is_string()) a.affiliation = j["affiliation"].get<std::string>();
    out.push_back(std::move(a));
  }
  return out;
}

namespace {

// The head tag of an analysis ("ok" when it has none).
Result<std::string> head_tag(ps::IStore& store, Uuid analysis) {
  auto head = store.head(analysis, ps::Kind::Tags);
  if (!head) return fail(head.error());
  if (!*head) return std::string("ok");
  auto payload = store.load_payload(**head);
  if (!payload) return fail(payload.error());
  if (!*payload) return std::string("ok");
  if (const auto* tag = std::get_if<ps::TagValue>(&**payload)) return tag->name;
  return std::string("ok");
}

Result<ps::ReleaseRow> resolve_supersedes(ps::IStore& store, const ReleaseSpec& spec) {
  if (spec.supersedes) return find_release(store, *spec.supersedes);
  auto newest = store.find_release(spec.name, std::nullopt);
  if (!newest) return fail(newest.error());
  if (!*newest) return fail(ErrorKind::Config, "no release named " + spec.name);
  return std::move(**newest);
}

}  // namespace

Result<ReleaseCreated> create_release(ps::IStore& store, const ps::Actor& actor, const ReleaseSpec& spec) {
  if (spec.name.empty()) return fail(ErrorKind::Config, "a release needs a name");
  if (spec.title.empty()) return fail(ErrorKind::Config, "a release needs a title");
  if (spec.licence.empty()) return fail(ErrorKind::Config, "a release needs a licence");

  // 1. Members: the candidates whose head tag is kept.
  std::set<std::string> kept(spec.tags.begin(), spec.tags.end());
  std::vector<Uuid> members;
  for (const auto& text : spec.analyses) {
    auto uuid = Uuid::parse(text);
    if (!uuid) return fail(ErrorKind::Config, "'" + text + "' is no analysis uuid");
    auto tag = head_tag(store, *uuid);
    if (!tag) return fail(tag.error());
    const bool keep = kept.empty() ? *tag != "invalid" : kept.contains(*tag);
    if (keep) members.push_back(*uuid);
  }
  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  if (members.empty()) return fail(ErrorKind::Config, "no analysis to release: none of the selection has a kept tag");

  // 2. Every head of every member, every resolved reference at its revision.
  std::vector<ps::HeadInfo> entries;
  std::map<Uuid, Uuid> refs;  // ref object -> revision
  std::set<Uuid> samples;
  for (const auto& a : members) {
    auto heads = store.heads(a);
    if (!heads) return fail(heads.error());
    if (heads->empty()) return fail(ErrorKind::Config, "analysis " + a.str() + " has no heads: not in the store");
    entries.insert(entries.end(), heads->begin(), heads->end());
    auto resolved = store.resolve_refs(a, ps::RefPolicy{});
    if (!resolved) return fail(resolved.error());
    for (const auto& r : resolved->refs) {
      const auto [it, inserted] = refs.emplace(r.ref_object, r.revision);
      if (!inserted && it->second != r.revision)
        return fail(ErrorKind::Config, "the selection's analyses disagree about the revision of reference " + r.key +
                                           " (one is pinned to another): a release pins one revision per reference");
    }
    auto detail = store.load_analysis_detail(a);
    if (!detail) return fail(detail.error());
    if (*detail && (*detail)->row.sample_uuid) samples.insert(*(*detail)->row.sample_uuid);
  }
  for (const auto& [object, revision] : refs) entries.push_back(ps::HeadInfo{object, ps::Kind::RefValue, revision, 1});

  // Interpreted ages: the named ones, or every one whose members are all in the release.
  std::vector<std::pair<Uuid, Uuid>> ias;
  if (!spec.interpreted_ages || !spec.interpreted_ages->empty()) {
    const std::set<Uuid> member_set(members.begin(), members.end());
    std::set<std::string> wanted;
    if (spec.interpreted_ages) wanted.insert(spec.interpreted_ages->begin(), spec.interpreted_ages->end());
    auto rows = store.interpreted_ages();
    if (!rows) return fail(rows.error());
    for (const auto& ia : *rows) {
      if (!ia.head) continue;
      if (spec.interpreted_ages) {
        if (!wanted.contains(ia.name)) continue;
      } else {
        auto payload = store.load_payload(*ia.head);
        if (!payload) return fail(payload.error());
        const auto* value = *payload ? std::get_if<ps::InterpretedAgeValue>(&**payload) : nullptr;
        if (!value || value->members.empty()) continue;
        const bool all_in = std::all_of(value->members.begin(), value->members.end(),
                                        [&](const ps::InterpretedAgeMember& m) { return member_set.contains(m.analysis); });
        if (!all_in) continue;
      }
      ias.emplace_back(ia.uuid, *ia.head);
      entries.push_back(ps::HeadInfo{ia.uuid, ps::Kind::InterpretedAge, *ia.head, 1});
    }
    if (spec.interpreted_ages && ias.size() != wanted.size())
      return fail(ErrorKind::Config, "not every named interpreted age exists with a value");
  }

  // Version and what it supersedes.
  int version = 1;
  std::optional<Uuid> supersedes;
  auto newest = store.find_release(spec.name, std::nullopt);
  if (!newest) return fail(newest.error());
  if (*newest) version = (*newest)->version + 1;
  if (spec.supersedes || *newest) {
    auto prior = resolve_supersedes(store, spec);
    if (!prior) return fail(prior.error());
    supersedes = prior->uuid;
  }

  // 3. The group and the bookmark; 4. the release row.
  const std::string tag = spec.name + "@v" + std::to_string(version);
  auto group = store.create_group(actor, "release:" + tag, members);
  if (!group) return fail(group.error());
  ps::BookmarkSpec bookmark{"release " + tag, "release " + spec.name + " v" + std::to_string(version), std::nullopt,
                            *group};
  bookmark.entries = entries;
  auto bm = store.create_bookmark(actor, bookmark);
  if (!bm) return fail(bm.error());

  ps::ReleaseInsert insert;
  insert.uuid = Uuid::v7();
  insert.name = spec.name;
  insert.title = spec.title;
  insert.description = spec.description;
  insert.version = version;
  insert.supersedes = supersedes;
  insert.group = *group;
  insert.bookmark = *bm;
  insert.licence = spec.licence;
  insert.authors_json = authors_json(spec.authors);
  insert.publisher = spec.publisher;
  insert.embargo_until = spec.embargo_until;
  insert.options_json = spec.options_json.empty() ? "{}" : spec.options_json;
  insert.reduction_version = spec.reduction_version;
  insert.software_version = spec.software_version;
  insert.samples.assign(samples.begin(), samples.end());
  insert.interpreted_ages = ias;
  auto id = store.add_release(actor, insert);
  if (!id) return fail(id.error());

  ReleaseCreated out;
  out.release = *id;
  out.version = version;
  out.members = members.size();
  out.samples = samples.size();
  out.interpreted_ages = ias.size();
  const UtcTime now = UtcTime::now();
  auto es = store.embargoed_samples(insert.samples, now);
  if (!es) return fail(es.error());
  out.embargoed_samples = es->size();
  auto ea = store.embargoed_analyses(members, now);
  if (!ea) return fail(ea.error());
  out.embargoed_analyses = ea->size();
  return out;
}

Result<void> publish_release(ps::IStore& store, const ps::Actor& actor, Uuid release,
                             const Sha256Digest& manifest_sha256, bool make_public) {
  ps::ReleaseUpdate u;
  u.release = release;
  u.state = ps::ReleaseState::Published;
  u.manifest_sha256 = manifest_sha256;
  if (make_public) u.visibility = ps::Visibility::Public;
  return store.update_release(actor, u);
}

Result<void> withdraw_release(ps::IStore& store, const ps::Actor& actor, Uuid release, std::string reason) {
  if (reason.empty()) return fail(ErrorKind::Config, "withdrawing a release needs a reason");
  ps::ReleaseUpdate u;
  u.release = release;
  u.state = ps::ReleaseState::Withdrawn;
  u.visibility = ps::Visibility::Private;
  u.withdrawn_reason = std::move(reason);
  return store.update_release(actor, u);
}

Result<void> set_release_public(ps::IStore& store, const ps::Actor& actor, Uuid release) {
  ps::ReleaseUpdate u;
  u.release = release;
  u.visibility = ps::Visibility::Public;
  return store.update_release(actor, u);
}

Result<void> delete_release(ps::IStore& store, const ps::Actor& actor, Uuid release) {
  return store.delete_release(actor, release);
}

Result<ps::ReleaseRow> find_release(ps::IStore& store, std::string_view name_or_uuid) {
  const std::string text(name_or_uuid);
  if (auto uuid = Uuid::parse(text)) {
    auto row = store.release(*uuid);
    if (!row) return fail(row.error());
    if (*row) return std::move(**row);
    return fail(ErrorKind::Config, "no release " + text);
  }
  std::string name = text;
  std::optional<int> version;
  if (const auto at = text.rfind("@v"); at != std::string::npos) {
    name = text.substr(0, at);
    const std::string v = text.substr(at + 2);
    // Digits only, and few enough for an int: std::stoi throws on more.
    if (v.empty() || v.size() > 9 || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }))
      return fail(ErrorKind::Config, "'" + text + "' is not <name>@v<version>");
    version = std::stoi(v);
  }
  auto row = store.find_release(name, version);
  if (!row) return fail(row.error());
  if (*row) return std::move(**row);
  return fail(ErrorKind::Config, "no release " + text);
}

}  // namespace pychron::publish
