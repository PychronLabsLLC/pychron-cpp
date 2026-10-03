// MetaRepoAdapter (meta_adapter.hpp). Three parts, as in the project adapter:
//
// Walk looks only at paths and blob shas: which changed files are reference
// files, and the versions each has had, in walk order. It reads no file, so
// replaying it over the commits an earlier run imported costs one diff
// listing, and gives a resumed walk the same "version before" for every file
// as an uninterrupted one.
//
// Mapper reads the files of one batch and turns them into catalog items,
// revisions and conflicts. It keeps nothing between batches and asks the
// store nothing: what a commit yields depends on the history alone, so the
// result is the same however the walk is cut, and a replay sends exactly the
// revisions that are stored (same ids), which the writer skips.
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
  std::string commit, path, blob_sha;
  MetaPath info;
  std::size_t version = 0;  // its place among the versions of its path (Walk::versions)
};

class Walk {
 public:
  // Applies the changes of commit `index` (for a merge: with what it kept of
  // its other parents). A path that already has the blob it is given is
  // skipped, which is what makes a merge repeat nothing. A deleted file adds
  // nothing and keeps its versions: added again as it was, it is not new.
  // With `out` null only the state moves.
  void apply(int index, std::span<const GitChange> changes, std::vector<Seen>* out) {
    for (const auto& entry : changes) {
      if (entry.status == 'D') continue;
      MetaPath info = classify_meta_path(entry.path);
      if (info.kind == MetaKind::Ignored) continue;
      auto& had = versions_[entry.path];
      if (!had.empty() && had.back() == entry.blob_sha) continue;
      had.push_back(entry.blob_sha);
      if (out) out->push_back({index, entry.commit, entry.path, entry.blob_sha, std::move(info), had.size() - 1});
    }
  }

  // The blobs a reference path has had, oldest first, no two neighbours equal.
  const std::vector<std::string>& versions(const std::string& path) const {
    static const std::vector<std::string> kNone;
    const auto it = versions_.find(path);
    return it == versions_.end() ? kNone : it->second;
  }

 private:
  std::unordered_map<std::string, std::vector<std::string>> versions_;
};

// Kinds whose file holds several objects and is compared with its version before.
bool is_compared(MetaKind kind) {
  return kind == MetaKind::Level || kind == MetaKind::LevelProductions || kind == MetaKind::Sensitivity;
}

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

class Mapper {
 public:
  // `config`, `reader` and `walk` outlive the mapper.
  Mapper(const MetaAdapterConfig& config, GitReader& reader, const Walk& walk)
      : config_(config), reader_(reader), walk_(walk) {}

  // Turns the reference files of one batch into its items. `commits` are the
  // batch's, the first being commit `first` of the walk.
  Result<void> map(const std::vector<Seen>& work, const std::vector<GitCommit>& commits, std::size_t first,
                   ingest::ImportBatch& batch) {
    // One git call for what is certainly read: each file, and the version
    // before it where the two are compared.
    std::vector<std::string> blobs;
    for (const auto& seen : work) {
      blobs.push_back(seen.blob_sha);
      if (is_compared(seen.info.kind) && seen.version > 0)
        blobs.push_back(walk_.versions(seen.path)[seen.version - 1]);
    }
    std::sort(blobs.begin(), blobs.end());
    blobs.erase(std::unique(blobs.begin(), blobs.end()), blobs.end());
    if (auto r = reader_.fetch_blobs(blobs); !r) return r;

    Output out{batch, commits, first, {}, {}};
    for (const auto& seen : work)
      if (auto r = file(seen, out); !r) return r;
    for (auto& entry : out.changesets) batch.changesets.push_back(std::move(entry.second));
    return {};
  }

 private:
  struct Output {
    ingest::ImportBatch& batch;
    const std::vector<GitCommit>& commits;
    std::size_t first;
    std::map<int, ingest::ChangesetItem> changesets;  // by commit index
    std::set<std::string> sent;                       // the catalog items this batch already has
  };

  // The bytes of a blob; the view is valid until the next read().
  Result<std::string_view> read(const std::string& blob_sha) {
    if (auto text = reader_.blob(blob_sha)) return text;
    const std::vector<std::string> one{blob_sha};
    if (auto r = reader_.fetch_blobs(one); !r) return fail(r.error());
    return reader_.blob(blob_sha);
  }

  // The version of a file before `seen` in the walk, parsed: the last one
  // that can be. nullopt: there is none.
  template <class T, class Parse>
  Result<std::optional<T>> previous(const Seen& seen, Parse&& parse) {
    const auto& earlier = walk_.versions(seen.path);
    for (std::size_t v = seen.version; v-- > 0;) {
      auto text = read(earlier[v]);
      if (!text) return fail(text.error());
      if (auto parsed = parse(*text)) return std::optional<T>{std::move(*parsed)};
    }
    return std::optional<T>{};
  }

  // ---------------------------------------------------------------- items

  void irradiation(const std::string& name, Output& out) {
    if (out.sent.insert("irradiation\n" + name).second) out.batch.catalog.push_back(ingest::IrradiationItem{name});
  }

  // The level, before anything scoped to it (batch.hpp: the first item sent
  // for a level decides its columns).
  void level(const std::string& irradiation_name, const std::string& name, std::optional<double> z, Output& out) {
    irradiation(irradiation_name, out);
    if (out.sent.insert("level\n" + irradiation_name + "\n" + name).second)
      out.batch.catalog.push_back(ingest::LevelItem{irradiation_name, name, std::nullopt, z, std::nullopt});
  }

  void object(ingest::RefObjectItem item, Output& out) {
    if (out.sent.insert("ref_object\n" + std::string(ps::to_string(item.type)) + "\n" + item.key).second)
      out.batch.catalog.push_back(std::move(item));
  }

  // A revision of object (type, key) from the file `seen`; `part` tells the
  // objects of one file apart ("#<position>") and is empty for a file that is
  // one object.
  void revision(const Seen& seen, const std::string& part, RefType type, const std::string& key,
                ps::RefPayload payload, const Json& detail, Output& out, std::string production_key = {}) {
    auto found = out.changesets.find(seen.index);
    if (found == out.changesets.end()) {
      const GitCommit& commit = out.commits[static_cast<std::size_t>(seen.index) - out.first];
      ingest::ChangesetItem changeset;
      changeset.commit = seen.commit;
      changeset.kind = ps::ChangesetKind::Reference;
      changeset.who = commit.author;
      changeset.message = commit.message;
      found = out.changesets.emplace(seen.index, std::move(changeset)).first;
    }
    ingest::RevisionItem item{{seen.commit, seen.path + part, seen.blob_sha},
                              ingest::RefObjectKey{std::string(ps::to_string(type)), key},
                              ps::Kind::RefValue,
                              ps::RevisionPayload{std::move(payload)},
                              dump(detail)};
    item.production_key = std::move(production_key);
    found->second.revisions.push_back(std::move(item));
  }

  // ---------------------------------------------------------------- files

  Result<void> file(const Seen& seen, Output& out) {
    auto text = read(seen.blob_sha);
    if (!text) return fail(text.error());
    // A reference file that cannot be read: a conflict, and the walk goes on.
    const auto unreadable = [&](const Error& error) -> Result<void> {
      out.batch.conflicts.push_back({{seen.commit, seen.path, seen.blob_sha},
                                     std::nullopt,
                                     ps::ConflictKind::Unparseable,
                                     sha256(*text),
                                     dump(Json{{"reason", error.what}})});
      return {};
    };
    const std::string& irradiation_name = seen.info.irradiation;
    const std::string& name = seen.info.name;

    switch (seen.info.kind) {
      case MetaKind::Level: {
        auto now = parse_level(*text);
        if (!now) return unreadable(now.error());
        auto before = previous<ParsedLevel>(seen, parse_level);
        if (!before) return fail(before.error());
        const std::string key = irradiation_name + "/" + name;
        ParsedLevelZ geometry = level_z_value(*now);
        level(irradiation_name, name, geometry.value.z, out);
        const bool same_header = *before && (*before)->header == now->header &&
                                 (*before)->header_nonfinite == now->header_nonfinite;
        if (!now->header.empty() && !same_header) {
          ingest::RefObjectItem item{RefType::LevelGeometry, key, irradiation_name, name, std::nullopt, std::nullopt};
          object(std::move(item), out);
          revision(seen, "#z", RefType::LevelGeometry, key, geometry.value, geometry.detail, out);
        }
        for (const auto& [hole, entries] : now->positions) {
          if (*before)
            if (const auto old = (*before)->positions.find(hole);
                old != (*before)->positions.end() && old->second == entries)
              continue;
          ParsedFlux flux = flux_value(entries);
          const std::string position = std::to_string(hole);
          object({RefType::FluxPosition, key + "/" + position, irradiation_name, name, hole, std::nullopt}, out);
          revision(seen, "#" + position, RefType::FluxPosition, key + "/" + position, std::move(flux.value),
                   flux.detail, out);
        }
        return {};
      }

      case MetaKind::LevelProductions: {
        auto now = parse_level_productions(*text);
        if (!now) return unreadable(now.error());
        auto before = previous<ParsedLevelProductions>(seen, parse_level_productions);
        if (!before) return fail(before.error());
        irradiation(irradiation_name, out);
        // The note and the values that name no production belong to the
        // file: when they change, every level is stated again.
        const bool same_rest = *before && (*before)->note == now->note && (*before)->extra == now->extra;
        for (const auto& [level_name, production] : now->levels) {
          if (same_rest)
            if (const auto old = (*before)->levels.find(level_name);
                old != (*before)->levels.end() && old->second == production)
              continue;
          const std::string key = irradiation_name + "/" + level_name;
          const std::string production_key = irradiation_name + "/" + production;
          level(irradiation_name, level_name, std::nullopt, out);
          object({RefType::Production, production_key, irradiation_name, std::nullopt, std::nullopt, std::nullopt},
                 out);
          object({RefType::LevelProduction, key, irradiation_name, level_name, std::nullopt, std::nullopt}, out);
          Json detail{{"production", production}};
          if (!now->extra.empty()) detail["extra"] = now->extra;
          // The writer resolves the production from its key.
          revision(seen, "#" + level_name, RefType::LevelProduction, key,
                   ps::LevelProductionValue{ps::Uuid{}, now->note}, detail, out, production_key);
        }
        return {};
      }

      case MetaKind::Production: {
        auto parsed = parse_frozen_production(*text, irradiation_name, {});
        if (!parsed) return unreadable(parsed.error());
        const std::string key = irradiation_name + "/" + name;
        irradiation(irradiation_name, out);
        object({RefType::Production, key, irradiation_name, std::nullopt, std::nullopt, std::nullopt}, out);
        Json detail = Json::object();
        if (parsed->name) detail["name"] = *parsed->name;
        if (parsed->extra_json)
          if (auto extra = parse_legacy(*parsed->extra_json)) detail["extra"] = std::move(*extra);
        revision(seen, "", RefType::Production, key, std::move(parsed->value), detail, out);
        return {};
      }

      case MetaKind::Chronology: {
        auto parsed = parse_chronology(*text, config_.lab_time_zone);
        if (!parsed) return unreadable(parsed.error());
        irradiation(irradiation_name, out);
        object({RefType::Chronology, irradiation_name, irradiation_name, std::nullopt, std::nullopt, std::nullopt},
               out);
        revision(seen, "", RefType::Chronology, irradiation_name, std::move(parsed->value), parsed->detail, out);
        return {};
      }

      case MetaKind::Gains: {
        auto parsed = parse_gains(*text);
        if (!parsed) return unreadable(parsed.error());
        const std::string spectrometer = lower(name);
        if (spectrometer != name) parsed->detail["name_as_written"] = name;
        object({RefType::Gains, spectrometer, std::nullopt, std::nullopt, std::nullopt, spectrometer}, out);
        revision(seen, "", RefType::Gains, spectrometer, std::move(parsed->value), parsed->detail, out);
        return {};
      }

      case MetaKind::Sensitivity: {
        auto now = parse_sensitivities(*text);
        if (!now) return unreadable(now.error());
        auto before = previous<std::vector<MetaEntry>>(seen, parse_sensitivities);
        if (!before) return fail(before.error());
        if (now->empty()) return {};
        const std::string spectrometer = lower(name);
        // The entries that are new or differ, in list order. The head must be
        // the last entry, the one the legacy code uses: it is stated again
        // when another revision of this commit, or an entry that is gone,
        // would be the head instead.
        std::vector<std::size_t> changed;
        for (std::size_t i = 0; i < now->size(); ++i)
          if (!*before || i >= (*before)->size() || (**before)[i] != (*now)[i]) changed.push_back(i);
        const std::size_t last = now->size() - 1;
        const bool restate = changed.empty() ? (*before)->back() != now->back() : changed.back() != last;
        if (restate) changed.push_back(last);
        if (changed.empty()) return {};
        object({RefType::Sensitivity, spectrometer, std::nullopt, std::nullopt, std::nullopt, spectrometer}, out);
        for (std::size_t n = 0; n < changed.size(); ++n) {
          ParsedSensitivity value = sensitivity_value((*now)[changed[n]], config_.lab_time_zone);
          if (restate && n + 1 == changed.size()) value.detail["restated"] = true;
          if (spectrometer != name) value.detail["name_as_written"] = name;
          revision(seen, "#" + std::to_string(changed[n]), RefType::Sensitivity, spectrometer,
                   std::move(value.value), value.detail, out);
        }
        return {};
      }

      case MetaKind::IrradiationHolder:
      case MetaKind::LoadHolder: {
        auto parsed = parse_holder(*text);
        if (!parsed) return unreadable(parsed.error());
        const RefType type =
            seen.info.kind == MetaKind::LoadHolder ? RefType::LoadHolder : RefType::IrradiationHolder;
        object({type, name, std::nullopt, std::nullopt, std::nullopt, std::nullopt}, out);
        revision(seen, "", type, name, std::move(parsed->value), parsed->detail, out);
        return {};
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
