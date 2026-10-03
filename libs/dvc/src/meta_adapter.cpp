// MetaRepoAdapter (meta_adapter.hpp). Three parts, as in the project adapter:
//
// Walk looks only at paths and blob shas: which changed files are reference
// files, and the versions each has had, in walk order, a deletion being a
// version too. It reads no file, so replaying it over the commits an earlier
// run imported costs one diff listing, and gives a resumed walk the same
// "version before" for every file as an uninterrupted one.
//
// Mapper reads the files of one batch and turns them into catalog items,
// revisions and conflicts. It keeps nothing between batches and asks the
// store nothing: what a commit yields depends on the history alone, so the
// result is the same however the walk is cut, and a replay sends exactly the
// revisions that are stored (same ids), which the writer skips.
//
// What a file version yields is the difference between the objects it holds
// and those of the version before it in the walk: a revision for each object
// that is new or differs, and for each that is gone a revision without a
// value ("removed" in its detail), so that the head never keeps a value the
// source no longer has (spec 10.21). Two payload types cannot say "no value"
// (a level_production must name a production, a sensitivity is a number):
// their removal is listed under "removed" in the commit's provenance detail
// and the head stays.
//
// The walk is one line through a history that is not. Where the version
// before a commit in the walk is not its parent's (a commit of another branch
// came between), part of the difference is not that commit's doing: an object
// the commit left as its parent had it is marked "walk": "branch". Every
// revision made at a merge is marked "walk": "merge".
//
// Impl holds the commit order, cuts the batches and writes the resume token
// (commit_walk.hpp).

#include "pychron/dvc/meta_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "commit_walk.hpp"
#include "legacy_json.hpp"
#include "meta_layout.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/dvc/legacy_layout.hpp"
#include "pychron/ingest/tz.hpp"

namespace pychron::dvc {

namespace ps = pychron::persistence;
using ps::RefType;

namespace {

// Commits whose changes are listed in one git call while replaying.
constexpr std::size_t kReplayChunk = 2000;

// A reference file as one commit left it.
struct Seen {
  int index = -1;  // the commit's place in the walk
  std::string commit, path;
  std::string blob_sha;     // empty: the commit deleted the file
  std::string parent_blob;  // the file at the commit's first parent; empty: not there
  MetaPath info;
  std::size_t version = 0;  // its place among the versions of its path (Walk::versions)
};

class Walk {
 public:
  // Applies the changes of commit `index` (for a merge: with what it kept of
  // its other parents). A path that already has the blob it is given, or is
  // deleted and already gone, is skipped, which is what makes a merge repeat
  // nothing. With `out` null only the state moves.
  void apply(int index, std::span<const GitChange> changes, std::vector<Seen>* out) {
    for (const auto& entry : changes) {
      MetaPath info = classify_meta_path(entry.path);
      if (info.kind == MetaKind::Ignored) continue;
      const bool deleted = entry.status == 'D';
      if (deleted && !versions_.contains(entry.path)) continue;
      auto& had = versions_[entry.path];
      if (deleted ? had.back().empty() : (!had.empty() && had.back() == entry.blob_sha)) continue;
      had.push_back(deleted ? std::string() : entry.blob_sha);
      if (out)
        out->push_back({index, entry.commit, entry.path, had.back(), entry.old_blob_sha, std::move(info),
                        had.size() - 1});
    }
  }

  // The blobs a reference path has had, oldest first, no two neighbours
  // equal; an empty one stands for a deletion.
  const std::vector<std::string>& versions(const std::string& path) const {
    static const std::vector<std::string> kNone;
    const auto it = versions_.find(path);
    return it == versions_.end() ? kNone : it->second;
  }

 private:
  std::unordered_map<std::string, std::vector<std::string>> versions_;
};

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// How a file version came to be the next in the walk.
struct Origin {
  bool merge = false;    // made by a merge commit
  bool crossed = false;  // the version before it in the walk is not its parent's
};

class Mapper {
 public:
  // `config`, `reader` and `walk` outlive the mapper.
  Mapper(const MetaAdapterConfig& config, GitReader& reader, const Walk& walk)
      : config_(config), reader_(reader), walk_(walk) {}

  // Turns the reference files of one batch into its items. `commits` are the
  // batch's, the first being commit `first` of the walk.
  Result<void> map(const std::vector<Seen>& work, const std::vector<GitCommit>& commits, std::size_t first,
                   ingest::ImportBatch& batch) {
    // One git call for what is certainly read: each file and the version before it.
    std::vector<std::string> blobs;
    for (const auto& seen : work) {
      if (!seen.blob_sha.empty()) blobs.push_back(seen.blob_sha);
      if (seen.version > 0 && !walk_.versions(seen.path)[seen.version - 1].empty())
        blobs.push_back(walk_.versions(seen.path)[seen.version - 1]);
    }
    std::sort(blobs.begin(), blobs.end());
    blobs.erase(std::unique(blobs.begin(), blobs.end()), blobs.end());
    if (auto r = reader_.fetch_blobs(blobs); !r) return r;

    Output out{batch, commits, first, {}, {}, {}};
    for (const auto& seen : work)
      if (auto r = file(seen, out); !r) return r;
    for (auto& [index, changeset] : out.changesets) {
      if (const auto gone = out.removed.find(index); gone != out.removed.end())
        changeset.detail_json = dump(Json{{"removed", gone->second}});
      batch.changesets.push_back(std::move(changeset));
    }
    return {};
  }

 private:
  struct Output {
    ingest::ImportBatch& batch;
    const std::vector<GitCommit>& commits;
    std::size_t first;
    std::map<int, ingest::ChangesetItem> changesets;  // by commit index
    std::map<int, Json> removed;                      // by commit index: what went and has no revision to say so
    std::set<std::string> sent;                       // the catalog items this batch already has
  };

  const GitCommit& commit_of(const Seen& seen, const Output& out) const {
    return out.commits[static_cast<std::size_t>(seen.index) - out.first];
  }

  // The bytes of a blob; the view is valid until the next read().
  Result<std::string_view> read(const std::string& blob_sha) {
    if (auto text = reader_.blob(blob_sha)) return text;
    const std::vector<std::string> one{blob_sha};
    if (auto r = reader_.fetch_blobs(one); !r) return fail(r.error());
    return reader_.blob(blob_sha);
  }

  // What the objects of a file hold before `seen`: the version before it in
  // the walk, parsed; when that one cannot be read, the one before it, and so
  // on. nullopt: nothing (no earlier version, or the file was deleted).
  template <class T, class Parse>
  Result<std::optional<T>> previous(const Seen& seen, Parse&& parse) {
    const auto& earlier = walk_.versions(seen.path);
    for (std::size_t v = seen.version; v-- > 0;) {
      if (earlier[v].empty()) break;
      auto text = read(earlier[v]);
      if (!text) return fail(text.error());
      if (auto parsed = parse(*text)) return std::optional<T>{std::move(*parsed)};
    }
    return std::optional<T>{};
  }

  Origin origin_of(const Seen& seen, const Output& out) const {
    Origin origin;
    origin.merge = commit_of(seen, out).parents.size() > 1;
    const std::string& before = seen.version > 0 ? walk_.versions(seen.path)[seen.version - 1] : seen.parent_blob;
    origin.crossed = !origin.merge && seen.version > 0 && before != seen.parent_blob;
    return origin;
  }

  // The file as the commit's parent had it, when that is not the version
  // before it in the walk: `T{}` when the parent had no such file. nullopt:
  // the two are the same version, or the parent's cannot be read; nothing is
  // then marked.
  template <class T, class Parse>
  Result<std::optional<T>> at_parent(const Seen& seen, const Origin& origin, Parse&& parse) {
    if (!origin.crossed) return std::optional<T>{};
    if (seen.parent_blob.empty()) return std::optional<T>{T{}};
    auto text = read(seen.parent_blob);
    if (!text) return fail(text.error());
    if (auto parsed = parse(*text)) return std::optional<T>{std::move(*parsed)};
    return std::optional<T>{};
  }

  // The provenance detail of a revision the walk made: `unchanged` says the
  // commit left the object as its parent had it.
  static Json marked(Json detail, const Origin& origin, bool unchanged) {
    if (origin.merge)
      detail["walk"] = "merge";
    else if (unchanged)
      detail["walk"] = "branch";
    return detail;
  }

  static Json removed() { return Json{{"removed", true}}; }

  // ---------------------------------------------------------------- items

  void irradiation(const std::string& name, Output& out) {
    if (out.sent.insert("irradiation\n" + name).second) out.batch.catalog.push_back(ingest::IrradiationItem{name});
  }

  // The level, before anything scoped to it. Its z is not sent: the
  // level_geometry object is the one place a level's z is kept.
  void level(const std::string& irradiation_name, const std::string& name, Output& out) {
    irradiation(irradiation_name, out);
    if (out.sent.insert("level\n" + irradiation_name + "\n" + name).second)
      out.batch.catalog.push_back(ingest::LevelItem{irradiation_name, name, std::nullopt, std::nullopt, std::nullopt});
  }

  void object(ingest::RefObjectItem item, Output& out) {
    if (out.sent.insert("ref_object\n" + std::string(ps::to_string(item.type)) + "\n" + item.key).second)
      out.batch.catalog.push_back(std::move(item));
  }

  ingest::ChangesetItem& changeset_of(const Seen& seen, Output& out) {
    auto found = out.changesets.find(seen.index);
    if (found == out.changesets.end()) {
      const GitCommit& commit = commit_of(seen, out);
      ingest::ChangesetItem changeset;
      changeset.commit = seen.commit;
      changeset.kind = ps::ChangesetKind::Reference;
      changeset.who = commit.author;
      changeset.message = commit.message;
      found = out.changesets.emplace(seen.index, std::move(changeset)).first;
    }
    return found->second;
  }

  // A revision of object (type, key) from the file `seen`; `part` tells the
  // objects of one file apart ("#<position>") and is empty for a file that is
  // one object.
  void revision(const Seen& seen, const std::string& part, RefType type, const std::string& key,
                ps::RefPayload payload, const Json& detail, Output& out, std::string production_key = {}) {
    ingest::RevisionItem item{{seen.commit, seen.path + part, seen.blob_sha},
                              ingest::RefObjectKey{std::string(ps::to_string(type)), key},
                              ps::Kind::RefValue,
                              ps::RevisionPayload{std::move(payload)},
                              dump(detail)};
    item.production_key = std::move(production_key);
    changeset_of(seen, out).revisions.push_back(std::move(item));
  }

  // An object that is gone and whose payload type cannot say so: named in the
  // provenance detail of the commit's changeset, which exists for that alone
  // when the commit has no revision.
  void note_removed(const Seen& seen, const std::string& part, Output& out) {
    changeset_of(seen, out);
    out.removed[seen.index].push_back(seen.path + part);
  }

  // ---------------------------------------------------------------- files

  // A file that is one object: `parse` gives its payload and detail. Deleted,
  // it leaves `Value{}`, when it had a value at all.
  template <class Value, class Parse>
  Result<void> whole(const Seen& seen, RefType type, const std::string& key, ingest::RefObjectItem item,
                     Parse&& parse, Output& out) {
    const Origin origin = origin_of(seen, out);
    if (seen.blob_sha.empty()) {
      auto before = previous<bool>(seen, [&](std::string_view text) -> Result<bool> {
        if (auto parsed = parse(text); !parsed) return fail(parsed.error());
        return true;
      });
      if (!before) return fail(before.error());
      if (*before) revision(seen, "", type, key, Value{}, marked(removed(), origin, false), out);
      return {};
    }
    auto text = read(seen.blob_sha);
    if (!text) return fail(text.error());
    auto parsed = parse(*text);
    if (!parsed) return unreadable(seen, *text, parsed.error(), out);
    if (item.irradiation) irradiation(*item.irradiation, out);
    object(std::move(item), out);
    revision(seen, "", type, key, std::move(parsed->first), marked(std::move(parsed->second), origin, false), out);
    return {};
  }

  // A reference file that cannot be read: a conflict, and the walk goes on.
  // Its objects keep what they have.
  Result<void> unreadable(const Seen& seen, std::string_view text, const Error& error, Output& out) {
    out.batch.conflicts.push_back({{seen.commit, seen.path, seen.blob_sha},
                                   std::nullopt,
                                   ps::ConflictKind::Unparseable,
                                   sha256(text),
                                   dump(Json{{"reason", error.what}})});
    return {};
  }

  Result<void> level_file(const Seen& seen, Output& out) {
    const std::string& irradiation_name = seen.info.irradiation;
    const std::string& name = seen.info.name;
    ParsedLevel now;  // a deleted file holds nothing
    if (!seen.blob_sha.empty()) {
      auto text = read(seen.blob_sha);
      if (!text) return fail(text.error());
      auto parsed = parse_level(*text);
      if (!parsed) return unreadable(seen, *text, parsed.error(), out);
      now = std::move(*parsed);
      level(irradiation_name, name, out);
    }
    auto before = previous<ParsedLevel>(seen, parse_level);
    if (!before) return fail(before.error());
    const Origin origin = origin_of(seen, out);
    auto parent = at_parent<ParsedLevel>(seen, origin, parse_level);
    if (!parent) return fail(parent.error());
    const auto same_header = [&](const std::optional<ParsedLevel>& other) {
      return other && other->header == now.header && other->header_nonfinite == now.header_nonfinite;
    };
    const auto same_position = [&](const std::optional<ParsedLevel>& other, int hole) {
      if (!other) return false;
      const auto theirs = other->positions.find(hole);
      const auto ours = now.positions.find(hole);
      if (theirs == other->positions.end() || ours == now.positions.end())
        return theirs == other->positions.end() && ours == now.positions.end();
      return theirs->second == ours->second;
    };
    const std::string key = irradiation_name + "/" + name;

    if (!now.header.empty() && !same_header(*before)) {
      ParsedLevelZ geometry = level_z_value(now);
      object({RefType::LevelGeometry, key, irradiation_name, name, std::nullopt, std::nullopt}, out);
      revision(seen, "#z", RefType::LevelGeometry, key, geometry.value,
               marked(std::move(geometry.detail), origin, same_header(*parent)), out);
    } else if (now.header.empty() && *before && !(*before)->header.empty()) {
      revision(seen, "#z", RefType::LevelGeometry, key, ps::LevelZValue{},
               marked(removed(), origin, same_header(*parent)), out);
    }
    for (const auto& [hole, entries] : now.positions) {
      if (same_position(*before, hole)) continue;
      ParsedFlux flux = flux_value(entries);
      const std::string position = std::to_string(hole);
      object({RefType::FluxPosition, key + "/" + position, irradiation_name, name, hole, std::nullopt}, out);
      revision(seen, "#" + position, RefType::FluxPosition, key + "/" + position, std::move(flux.value),
               marked(std::move(flux.detail), origin, same_position(*parent, hole)), out);
    }
    if (*before)
      for (const auto& [hole, entries] : (*before)->positions) {
        if (now.positions.contains(hole)) continue;
        const std::string position = std::to_string(hole);
        revision(seen, "#" + position, RefType::FluxPosition, key + "/" + position, ps::FluxValue{},
                 marked(removed(), origin, same_position(*parent, hole)), out);
      }
    return {};
  }

  Result<void> level_productions(const Seen& seen, Output& out) {
    const std::string& irradiation_name = seen.info.irradiation;
    ParsedLevelProductions now;
    if (!seen.blob_sha.empty()) {
      auto text = read(seen.blob_sha);
      if (!text) return fail(text.error());
      auto parsed = parse_level_productions(*text);
      if (!parsed) return unreadable(seen, *text, parsed.error(), out);
      now = std::move(*parsed);
      irradiation(irradiation_name, out);
    }
    auto before = previous<ParsedLevelProductions>(seen, parse_level_productions);
    if (!before) return fail(before.error());
    const Origin origin = origin_of(seen, out);
    auto parent = at_parent<ParsedLevelProductions>(seen, origin, parse_level_productions);
    if (!parent) return fail(parent.error());
    // The note and the values that name no production belong to the file:
    // when they change, every level is stated again.
    const auto same_level = [&](const std::optional<ParsedLevelProductions>& other, const std::string& level_name) {
      if (!other || other->note != now.note || other->extra != now.extra) return false;
      const auto theirs = other->levels.find(level_name);
      return theirs != other->levels.end() && theirs->second == now.levels.at(level_name);
    };
    for (const auto& [level_name, production] : now.levels) {
      if (same_level(*before, level_name)) continue;
      const std::string key = irradiation_name + "/" + level_name;
      const std::string production_key = irradiation_name + "/" + production;
      level(irradiation_name, level_name, out);
      object({RefType::Production, production_key, irradiation_name, std::nullopt, std::nullopt, std::nullopt}, out);
      object({RefType::LevelProduction, key, irradiation_name, level_name, std::nullopt, std::nullopt}, out);
      Json detail{{"production", production}};
      if (!now.extra.empty()) detail["extra"] = now.extra;
      // The writer resolves the production from its key.
      revision(seen, "#" + level_name, RefType::LevelProduction, key, ps::LevelProductionValue{ps::Uuid{}, now.note},
               marked(std::move(detail), origin, same_level(*parent, level_name)), out, production_key);
    }
    // A level_production value must name a production: a level that is gone
    // keeps its head, and the commit says so.
    if (*before)
      for (const auto& [level_name, production] : (*before)->levels)
        if (!now.levels.contains(level_name)) note_removed(seen, "#" + level_name, out);
    return {};
  }

  Result<void> sensitivities(const Seen& seen, Output& out) {
    const std::string& name = seen.info.name;
    std::vector<MetaEntry> now;
    if (!seen.blob_sha.empty()) {
      auto text = read(seen.blob_sha);
      if (!text) return fail(text.error());
      auto parsed = parse_sensitivities(*text);
      if (!parsed) return unreadable(seen, *text, parsed.error(), out);
      now = std::move(*parsed);
    }
    auto before = previous<std::vector<MetaEntry>>(seen, parse_sensitivities);
    if (!before) return fail(before.error());
    if (now.empty()) {
      // A sensitivity value is a number and cannot say "none": the head
      // stays, and the commit says the list went.
      if (*before && !(*before)->empty()) note_removed(seen, "", out);
      return {};
    }
    const Origin origin = origin_of(seen, out);
    auto parent = at_parent<std::vector<MetaEntry>>(seen, origin, parse_sensitivities);
    if (!parent) return fail(parent.error());
    const std::string spectrometer = lower(name);
    // The entries that are new or differ, in list order. The head must be the
    // last entry, the one the legacy code uses: it is stated again when
    // another revision of this commit, or an entry that is gone, would be the
    // head instead.
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < now.size(); ++i)
      if (!*before || i >= (*before)->size() || (**before)[i] != now[i]) changed.push_back(i);
    const std::size_t last = now.size() - 1;
    const bool restate = changed.empty() ? (*before)->back() != now.back() : changed.back() != last;
    if (restate) changed.push_back(last);
    if (changed.empty()) return {};
    object({RefType::Sensitivity, spectrometer, std::nullopt, std::nullopt, std::nullopt, spectrometer}, out);
    for (std::size_t n = 0; n < changed.size(); ++n) {
      const std::size_t i = changed[n];
      ParsedSensitivity value = sensitivity_value(now[i], config_.lab_time_zone);
      if (restate && n + 1 == changed.size()) value.detail["restated"] = true;
      if (spectrometer != name) value.detail["name_as_written"] = name;
      const bool unchanged = *parent && i < (*parent)->size() && (**parent)[i] == now[i];
      revision(seen, "#" + std::to_string(i), RefType::Sensitivity, spectrometer, std::move(value.value),
               marked(std::move(value.detail), origin, unchanged), out);
    }
    return {};
  }

  Result<void> file(const Seen& seen, Output& out) {
    const std::string& irradiation_name = seen.info.irradiation;
    const std::string& name = seen.info.name;
    switch (seen.info.kind) {
      case MetaKind::Level:
        return level_file(seen, out);
      case MetaKind::LevelProductions:
        return level_productions(seen, out);
      case MetaKind::Sensitivity:
        return sensitivities(seen, out);

      case MetaKind::Production: {
        const std::string key = irradiation_name + "/" + name;
        const auto parse = [&](std::string_view text) -> Result<std::pair<ps::ProductionValue, Json>> {
          auto parsed = parse_frozen_production(text, irradiation_name, {});
          if (!parsed) return fail(parsed.error());
          Json detail = Json::object();
          if (parsed->name) detail["name"] = *parsed->name;
          if (parsed->extra_json)
            if (auto extra = parse_legacy(*parsed->extra_json)) detail["extra"] = std::move(*extra);
          return std::pair{std::move(parsed->value), std::move(detail)};
        };
        return whole<ps::ProductionValue>(
            seen, RefType::Production, key,
            {RefType::Production, key, irradiation_name, std::nullopt, std::nullopt, std::nullopt}, parse, out);
      }

      case MetaKind::Chronology: {
        const auto parse = [&](std::string_view text) -> Result<std::pair<ps::ChronologyValue, Json>> {
          auto parsed = parse_chronology(text, config_.lab_time_zone);
          if (!parsed) return fail(parsed.error());
          return std::pair{std::move(parsed->value), std::move(parsed->detail)};
        };
        return whole<ps::ChronologyValue>(
            seen, RefType::Chronology, irradiation_name,
            {RefType::Chronology, irradiation_name, irradiation_name, std::nullopt, std::nullopt, std::nullopt},
            parse, out);
      }

      case MetaKind::Gains: {
        const std::string spectrometer = lower(name);
        const auto parse = [&](std::string_view text) -> Result<std::pair<ps::GainsValue, Json>> {
          auto parsed = parse_gains(text);
          if (!parsed) return fail(parsed.error());
          if (spectrometer != name) parsed->detail["name_as_written"] = name;
          return std::pair{std::move(parsed->value), std::move(parsed->detail)};
        };
        return whole<ps::GainsValue>(
            seen, RefType::Gains, spectrometer,
            {RefType::Gains, spectrometer, std::nullopt, std::nullopt, std::nullopt, spectrometer}, parse, out);
      }

      case MetaKind::IrradiationHolder:
      case MetaKind::LoadHolder: {
        const RefType type =
            seen.info.kind == MetaKind::LoadHolder ? RefType::LoadHolder : RefType::IrradiationHolder;
        const auto parse = [](std::string_view text) -> Result<std::pair<ps::HolderValue, Json>> {
          auto parsed = parse_holder(text);
          if (!parsed) return fail(parsed.error());
          return std::pair{std::move(parsed->value), std::move(parsed->detail)};
        };
        return whole<ps::HolderValue>(seen, type, name,
                                      {type, name, std::nullopt, std::nullopt, std::nullopt, std::nullopt}, parse,
                                      out);
      }

      case MetaKind::Ignored:
        break;
    }
    return {};
  }

  const MetaAdapterConfig& config_;
  GitReader& reader_;
  const Walk& walk_;
};

}  // namespace

class MetaRepoAdapter::Impl {
 public:
  Impl(MetaAdapterConfig config, GitReader reader) : config_(std::move(config)), reader_(std::move(reader)) {
    if (config_.batch_commits < 1) config_.batch_commits = 1;
  }

  Result<ingest::SourceDescription> describe() const {
    return ingest::SourceDescription{ps::ImportSourceKind::MetaRepo, config_.url, config_.git.branch, reader_.head()};
  }

  Result<int> plan(const std::optional<std::string>& resume_token) {
    walk_ = Walk{};
    order_.clear();
    planned_ = false;
    first_ = next_ = 0;

    auto all = reader_.rev_list(std::nullopt);
    if (!all) return fail(all.error());
    order_ = std::move(*all);
    const auto resume = detail::resume_point(order_, resume_token, reader_, config_.git, "meta adapter");
    if (!resume) return fail(resume.error());
    first_ = next_ = resume->first;
    planned_ = true;
    if (first_ == order_.size()) return 0;  // nothing new: no batch will be asked for

    // The versions each path had before the token, without reading a file.
    for (std::size_t begin = 0; begin < first_; begin += kReplayChunk)
      if (auto r = walk(begin, std::min(first_, begin + kReplayChunk), nullptr, nullptr); !r) return fail(r.error());
    return static_cast<int>(order_.size() - first_);
  }

  Result<std::optional<ingest::ImportBatch>> next_batch() {
    if (!planned_) return fail(ErrorKind::Config, "meta adapter: next_batch() before plan()");
    if (next_ == order_.size()) return std::optional<ingest::ImportBatch>{};

    ingest::ImportBatch batch;
    batch.head = reader_.head();
    batch.total = static_cast<int>(order_.size());
    const std::size_t end = std::min(order_.size(), next_ + static_cast<std::size_t>(config_.batch_commits));
    std::vector<Seen> work;
    std::vector<GitCommit> commits;
    if (auto r = walk(next_, end, &work, &commits); !r) return fail(r.error());
    Mapper mapper(config_, reader_, walk_);
    if (auto r = mapper.map(work, commits, next_, batch); !r) return fail(r.error());

    next_ = end;
    batch.done = static_cast<int>(next_);
    batch.resume_token = detail::format_token(order_, next_ - 1, next_ == order_.size());
    return std::optional<ingest::ImportBatch>{std::move(batch)};
  }

 private:
  // Applies commits [begin, end) of the walk order. `out` null: a replay.
  Result<void> walk(std::size_t begin, std::size_t end, std::vector<Seen>* out, std::vector<GitCommit>* commits) {
    std::vector<GitCommit> listed;
    auto applied = detail::walk_commits(reader_, order_, begin, end, listed,
                                        [&](std::size_t index, std::span<const GitChange> changes) {
                                          walk_.apply(static_cast<int>(index), changes, out);
                                          return false;
                                        });
    if (!applied) return fail(applied.error());
    if (commits) *commits = std::move(listed);
    return {};
  }

  MetaAdapterConfig config_;
  GitReader reader_;

  bool planned_ = false;
  std::vector<std::string> order_;  // commits earlier runs walked, then those to walk
  std::size_t first_ = 0;           // the first commit to walk
  std::size_t next_ = 0;
  Walk walk_;
};

MetaRepoAdapter::MetaRepoAdapter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetaRepoAdapter::~MetaRepoAdapter() = default;

Result<std::unique_ptr<MetaRepoAdapter>> MetaRepoAdapter::open(MetaAdapterConfig config) {
  if (config.url.empty()) return fail(ErrorKind::Config, "meta adapter: no source url");
  if (!ingest::known_zone(config.lab_time_zone))
    return fail(ErrorKind::Config, "meta adapter: unknown time zone '" + config.lab_time_zone + "'");
  auto reader = GitReader::open(config.git);
  if (!reader) return fail(reader.error());
  return std::unique_ptr<MetaRepoAdapter>(
      new MetaRepoAdapter(std::make_unique<Impl>(std::move(config), std::move(*reader))));
}

Result<ingest::SourceDescription> MetaRepoAdapter::describe() { return impl_->describe(); }

Result<int> MetaRepoAdapter::plan(std::optional<std::string> resume_token, ingest::IImportState&) {
  return impl_->plan(resume_token);
}

Result<std::optional<ingest::ImportBatch>> MetaRepoAdapter::next_batch() { return impl_->next_batch(); }

}  // namespace pychron::dvc
