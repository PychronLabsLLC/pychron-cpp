#include "pychron/ingest/writer.hpp"

#include <cstdio>
#include <set>
#include <utility>
#include <vector>

#include "catalog.hpp"
#include "pychron/ingest/ids.hpp"

namespace pychron::ingest {

namespace P = pychron::persistence;
using P::Kind;
using P::Uuid;

namespace {

constexpr const char* kRunning = "running";
constexpr const char* kPaused = "paused";
constexpr const char* kFinished = "finished";
constexpr const char* kFailed = "failed";
constexpr const char* kPending = "pending";
constexpr const char* kSuperseded = "superseded";

// Calls exactly one lambda per alternative. There is no catch-all, so a new
// SubjectRef alternative does not compile until every visit here handles it.
template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

// The six root revisions of an analysis: its kind, the file it came from and
// the id slot in the ingest.
struct Root {
  Kind kind;
  SourceKey RootKeys::* key;
  Uuid P::CollectionRoots::* id;
};
constexpr Root kRoots[] = {
    {Kind::Signals, &RootKeys::signals, &P::CollectionRoots::signals},
    {Kind::Intercepts, &RootKeys::intercepts, &P::CollectionRoots::intercepts},
    {Kind::Baselines, &RootKeys::baselines, &P::CollectionRoots::baselines},
    {Kind::Blanks, &RootKeys::blanks, &P::CollectionRoots::blanks},
    {Kind::IcFactors, &RootKeys::icfactors, &P::CollectionRoots::icfactors},
    {Kind::Tags, &RootKeys::tags, &P::CollectionRoots::tags},
};

std::string json_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

using JsonMembers = std::vector<std::pair<std::string, std::string>>;  // name -> JSON value text

std::string json_object(const JsonMembers& members) {
  std::string out = "{";
  for (const auto& [name, value] : members) {
    if (out.size() > 1) out += ',';
    out += json_string(name) + ":" + value;
  }
  return out + "}";
}

// `object` with `members` appended. Text that is not a JSON object is kept as
// a string under "detail" rather than spliced into.
std::string with_members(std::string_view object, const JsonMembers& members) {
  if (members.empty()) return std::string(object);
  constexpr std::string_view space = " \t\r\n";
  const auto first = object.find_first_not_of(space);
  const auto last = object.find_last_not_of(space);
  if (first == std::string_view::npos || first == last || object[first] != '{' || object[last] != '}') {
    JsonMembers all{{"detail", json_string(object)}};
    all.insert(all.end(), members.begin(), members.end());
    return json_object(all);
  }
  const auto inner = object.substr(first + 1, last - first - 1);
  std::string out = "{" + std::string(inner);
  if (inner.find_first_not_of(space) != std::string_view::npos) out += ',';
  return out + json_object(members).substr(1);
}

std::string author_text(const GitWho& who) { return who.name + " <" + who.email + ">"; }

P::ProvenanceRow provenance(const char* entity_type, Uuid entity, const SourceKey& key, const GitWho& who,
                            std::optional<std::string> detail_json = std::nullopt) {
  return {entity_type, entity, key.path, key.commit, key.blob_sha, author_text(who), who.utc, std::move(detail_json)};
}

std::string bookmark_path(const BookmarkItem& bookmark) { return "refs/tags/" + bookmark.name; }

// What a batch adds to the import transaction.
struct Staged {
  std::vector<P::ImportedChangeset> changesets;
  std::vector<P::ProvenanceRow> provenance;
  std::vector<P::ImportConflictRow> conflicts;
  std::vector<std::pair<Uuid, const char*>> resolutions;  // conflict -> new resolution
};

}  // namespace

class BatchWriter::Impl final : public IImportState {
 public:
  Impl(P::IStore& store, Uuid client, WriterConfig config)
      : store_(store), client_(client), config_(std::move(config)), catalog_(store, client) {}

  Result<P::ImportSourceInfo> open(ISourceAdapter& adapter) {
    auto described = adapter.describe();
    if (!described) return fail(described.error());
    url_ = normalize_source_url(described->url);

    P::ImportSourceSpec spec;
    spec.uuid = source_id(described->kind, url_, described->branch);
    spec.kind = described->kind;
    spec.url_or_path = url_;
    if (!described->branch.empty()) spec.branch = described->branch;
    spec.importer_version = config_.importer_version;
    spec.lab_time_zone = config_.lab_time_zone;

    P::ImportSourceInfo info;
    if (config_.dry_run) {
      // Registering is a write: a source that is not stored is described as it would be.
      auto all = store_.import_sources();
      if (!all) return fail(all.error());
      info.spec = spec;
      info.status = "registered";
      for (auto& stored : *all)
        if (stored.spec.uuid == spec.uuid) info = std::move(stored);
    } else {
      auto stored = store_.begin_import(spec);
      if (!stored) return fail(stored.error());
      info = std::move(*stored);
    }
    if (info.progress_token && info.progress_token->empty()) info.progress_token.reset();

    source_ = info.spec.uuid;
    token_ = info.progress_token;
    done_ = info.done;
    total_ = info.total;
    head_.reset();
    if (!described->head.empty()) head_ = described->head;
    return info;
  }

  Result<RunStats> run(ISourceAdapter& adapter, std::optional<int> max_batches, const std::function<bool()>& keep_going,
                       const std::function<void(const RunStats&, const ImportBatch&)>& on_batch) {
    if (!source_)
      if (auto opened = open(adapter); !opened) return fail(opened.error());
    RunStats stats;
    if (auto r = run_batches(adapter, max_batches, keep_going, on_batch, stats); !r) {
      conflicts_.reset();         // staged changes to it may not have been stored
      (void)set_status(kFailed);  // best effort: the error that stopped the run is the one reported
      return fail(r.error());
    }
    return stats;
  }

  // ------------------------------------------------------------ IImportState

  Result<std::optional<std::string>> head_blob_sha(const SubjectRef& subject, Kind kind) override {
    if (!source_) return fail(ErrorKind::Config, "import state: no source is open");
    auto uuid = std::visit(Overloaded{
                               [](const Uuid& analysis) -> Result<Uuid> { return analysis; },
                               [&](const RefObjectKey& key) { return catalog_.ref_object_id(key); },
                           },
                           subject);
    if (!uuid) return fail(uuid.error());
    return store_.imported_head_blob_sha(*source_, *uuid, kind);
  }

  Result<bool> analysis_exists(Uuid analysis) override {
    auto view = store_.load_analysis(analysis);
    if (!view) return fail(view.error());
    return view->has_value();
  }

 private:
  struct Author {
    Uuid user;
    std::string name;
  };
  struct Members {
    Uuid added_by;
    std::vector<Uuid> analyses;
  };
  using Memberships = std::map<std::string, Members>;  // by repository name

  Result<void> run_batches(ISourceAdapter& adapter, std::optional<int> max_batches,
                           const std::function<bool()>& keep_going,
                           const std::function<void(const RunStats&, const ImportBatch&)>& on_batch, RunStats& stats) {
    // A replay walks from the start. Until the walk reaches the stored token,
    // the batches it writes leave the stored progress as it is.
    catching_up_ = config_.replay && token_.has_value();
    std::optional<P::ImportProgress> walked;  // of the last batch written while catching up
    if (auto planned = adapter.plan(config_.replay ? std::nullopt : token_, *this); !planned)
      return fail(planned.error());
    for (;;) {
      if (max_batches && stats.batches >= *max_batches) return set_status(kPaused);
      auto next = adapter.next_batch();
      if (!next) return fail(next.error());
      if (!*next) {
        stats.finished = true;
        // The whole source was walked without meeting the stored token (the
        // batches are cut differently now): the end of the walk is past it.
        if (catching_up_ && walked) adopt(*walked);
        return set_status(kFinished);
      }
      const ImportBatch& batch = **next;
      if (auto r = config_.dry_run ? count_batch(batch, stats) : write_batch(batch, stats, walked); !r) return r;
      ++stats.batches;
      if (on_batch) on_batch(stats, batch);
      if (keep_going && !keep_going()) return set_status(kPaused);
    }
  }

  P::ImportProgress progress(const char* status) const { return {token_.value_or(""), done_, total_, head_, status}; }

  void adopt(const P::ImportProgress& committed) {
    if (!committed.token.empty()) token_ = committed.token;
    done_ = committed.done;
    total_ = committed.total;
    head_ = committed.head_sha;
  }

  // Restates the stored progress with a new status.
  Result<void> set_status(const char* status) {
    if (config_.dry_run) return {};
    auto uow = store_.begin_import_batch(*source_, client_);
    if (!uow) return fail(uow.error());
    if (auto r = (*uow)->set_progress(progress(status)); !r) return r;
    if (auto seq = (*uow)->commit(); !seq) return fail(seq.error());
    return {};
  }

  // A git author as an app_user: the mapped user, else "git:<email>".
  Result<Author> author(const GitWho& who) {
    const std::string& id = who.email.empty() ? who.name : who.email;
    if (auto it = authors_.find(id); it != authors_.end()) return it->second;
    const auto mapped = config_.author_map.find(who.email);
    Author out;
    out.name = mapped != config_.author_map.end() ? mapped->second : "git:" + id;
    auto user = catalog_.user(out.name);
    if (!user) return fail(user.error());
    out.user = *user;
    authors_.emplace(id, out);
    return out;
  }

  Result<bool> analysis_present(Uuid analysis) {
    if (present_.contains(analysis)) return true;
    auto heads = store_.heads(analysis);
    if (!heads) return fail(heads.error());
    if (heads->empty()) return false;
    present_.insert(analysis);
    return true;
  }

  // nullopt: the subject is an analysis that is not in the store.
  Result<std::optional<Uuid>> resolve(const SubjectRef& subject, Kind kind) {
    return std::visit(Overloaded{
                          [&](const Uuid& uuid) -> Result<std::optional<Uuid>> {
                            if (P::subject_type_of(kind) != P::SubjectType::Analysis) return std::optional<Uuid>{uuid};
                            auto present = analysis_present(uuid);
                            if (!present) return fail(present.error());
                            return *present ? std::optional<Uuid>{uuid} : std::nullopt;
                          },
                          [&](const RefObjectKey& key) -> Result<std::optional<Uuid>> {
                            auto uuid = catalog_.ref_object(key);
                            if (!uuid) return fail(uuid.error());
                            return std::optional<Uuid>{*uuid};
                          },
                      },
                      subject);
  }

  // ------------------------------------------------------------ conflicts

  struct KnownConflict {
    P::ConflictKind kind;
    std::string resolution;
  };

  // Every conflict stored for this source, by id; kept current with what the
  // writer stages. Loaded once per run.
  Result<std::map<Uuid, KnownConflict>*> conflicts() {
    if (!conflicts_) {
      auto rows = store_.import_conflicts({*source_, std::nullopt, std::nullopt});
      if (!rows) return fail(rows.error());
      conflicts_.emplace();
      for (const auto& row : *rows) conflicts_->emplace(row.uuid, KnownConflict{row.kind, row.resolution});
    }
    return &*conflicts_;
  }

  // Stages a conflict row. One that is stored and resolved stays resolved and
  // is not counted.
  Result<void> stage_conflict(P::ImportConflictRow row, Staged& staged, RunStats& stats) {
    auto known = conflicts();
    if (!known) return fail(known.error());
    const auto [it, added] = (*known)->try_emplace(row.uuid, KnownConflict{row.kind, kPending});
    if (added || it->second.resolution == kPending) ++stats.conflicts;
    staged.conflicts.push_back(std::move(row));
    return {};
  }

  // An analysis, or a revision of one, that cannot be written because the
  // analysis is not in the store. The writer has no file bytes: the conflict
  // carries the SHA-256 of the git blob sha text. A conflict that was
  // superseded and applies again is pending again.
  Result<void> stage_unknown_analysis(const SourceKey& key, Uuid analysis, std::string_view reason, Staged& staged,
                                      RunStats& stats) {
    const Uuid id = conflict_id(url_, key.commit, key.path);
    auto known = conflicts();
    if (!known) return fail(known.error());
    if (auto it = (*known)->find(id);
        it != (*known)->end() && it->second.kind == P::ConflictKind::UnknownAnalysis &&
        it->second.resolution == kSuperseded) {
      it->second.resolution = kPending;
      staged.resolutions.emplace_back(id, kPending);
    }
    return stage_conflict({id, key.path, analysis, P::ConflictKind::UnknownAnalysis, std::nullopt,
                           sha256(std::string_view{key.blob_sha}), json_object({{"reason", json_string(reason)}}),
                           kPending},
                          staged, stats);
  }

  // The file at `key` is now written: a pending unknown_analysis conflict
  // about it no longer applies.
  Result<void> supersede(const SourceKey& key, Staged& staged) {
    auto known = conflicts();
    if (!known) return fail(known.error());
    const Uuid id = conflict_id(url_, key.commit, key.path);
    auto it = (*known)->find(id);
    if (it == (*known)->end() || it->second.kind != P::ConflictKind::UnknownAnalysis ||
        it->second.resolution != kPending)
      return {};
    it->second.resolution = kSuperseded;
    staged.resolutions.emplace_back(id, kSuperseded);
    return {};
  }

  // The files of an analysis's collection: the record and each root that has one.
  std::vector<SourceKey> collection_files(const AnalysisItem& item) const {
    std::vector<SourceKey> files{item.keys.record};
    for (const auto& root : kRoots)
      if (SourceKey key = root_key(item, root); !key.path.empty()) files.push_back(std::move(key));
    return files;
  }

  // ------------------------------------------------------------ writing

  Result<void> write_batch(const ImportBatch& batch, RunStats& stats, std::optional<P::ImportProgress>& walked) {
    // Step 1: writes that are idempotent on their own.
    for (const auto& item : batch.catalog)
      if (auto r = catalog_.write(item); !r) return r;
    // Blobs go first so each analysis is ingested with its signals complete.
    for (const auto& item : batch.blobs) {
      auto ack = store_.ingest({revision_id(url_, item.key.commit, item.key.path),
                                P::blob_sha256(item.blob.codec, item.blob.bytes), client_, item.blob});
      if (!ack) return fail(ack.error());
    }
    Staged staged;
    Memberships memberships;
    for (const auto& item : batch.analyses)
      if (auto r = write_analysis(item, staged, memberships, stats); !r) return r;
    for (const auto& item : batch.memberships)
      if (auto r = stage_membership(item, staged, memberships, stats); !r) return r;
    for (const auto& [name, members] : memberships) {
      auto repository = catalog_.repository(name);
      if (!repository) return fail(repository.error());
      if (auto r = store_.add_repository_members({members.added_by, client_}, *repository, members.analyses); !r)
        return r;
    }

    // Step 2: one transaction.
    for (const auto& item : batch.changesets)
      if (auto r = stage_changeset(item, staged, stats); !r) return r;
    for (const auto& item : batch.conflicts)
      if (auto r = stage_conflict({conflict_id(url_, item.key.commit, item.key.path), item.key.path, item.entity,
                                   item.kind, std::nullopt, item.file_sha256, item.detail_json, kPending},
                                  staged, stats);
          !r)
        return r;

    P::ImportProgress reached = progress(kRunning);
    if (!batch.resume_token.empty()) reached.token = batch.resume_token;
    reached.done = batch.done;
    reached.total = batch.total;
    if (!batch.head.empty()) reached.head_sha = batch.head;
    // While a replay catches up, the stored progress is restated unchanged.
    const P::ImportProgress next = catching_up_ ? progress(kRunning) : reached;

    // A bookmark captures heads, so it is made after the batch's revisions are
    // stored; the token moves only once the bookmarks are recorded too.
    const bool has_bookmarks = !batch.bookmarks.empty();
    if (auto r = commit(std::move(staged), has_bookmarks ? nullptr : &next); !r) return r;
    if (has_bookmarks) {
      Staged recorded;
      for (const auto& item : batch.bookmarks)
        if (auto r = write_bookmark(item, recorded); !r) return r;
      if (auto r = commit(std::move(recorded), &next); !r) return r;
    }

    if (catching_up_) {
      walked = reached;
      if (token_ && reached.token == *token_) catching_up_ = false;  // from here on the token advances
    } else {
      adopt(next);
    }
    return {};
  }

  Result<void> commit(Staged staged, const P::ImportProgress* next) {
    auto uow = store_.begin_import_batch(*source_, client_);
    if (!uow) return fail(uow.error());
    for (auto& changeset : staged.changesets)
      if (auto r = (*uow)->add_changeset(std::move(changeset)); !r) return r;
    for (auto& row : staged.provenance)
      if (auto r = (*uow)->add_provenance(std::move(row)); !r) return r;
    for (auto& row : staged.conflicts)
      if (auto r = (*uow)->add_conflict(std::move(row)); !r) return r;
    for (const auto& [conflict, resolution] : staged.resolutions)
      if (auto r = (*uow)->resolve_conflict(conflict, resolution); !r) return r;
    if (next)
      if (auto r = (*uow)->set_progress(*next); !r) return r;
    if (auto seq = (*uow)->commit(); !seq) return fail(seq.error());
    return {};
  }

  // The file of a root revision, and the id derived from it. A kind with no
  // file is keyed by the record path and the kind.
  SourceKey root_key(const AnalysisItem& item, const Root& root) const {
    SourceKey key = item.keys.*root.key;
    if (key.commit.empty()) key.commit = item.keys.record.commit;
    return key;
  }
  Uuid root_id(const AnalysisItem& item, const Root& root) const {
    const SourceKey key = root_key(item, root);
    if (!key.path.empty()) return revision_id(url_, key.commit, key.path);
    return revision_id(url_, item.keys.record.commit,
                       item.keys.record.path + "#" + std::string(P::to_string(root.kind)));
  }

  std::string analysis_detail(const AnalysisItem& item) const {
    JsonMembers members;
    if (item.synthetic_collection) members.emplace_back("synthetic_collection", "true");
    JsonMembers commits;
    for (const auto& root : kRoots) {
      const SourceKey key = root_key(item, root);
      if (!key.path.empty() && key.commit != item.keys.record.commit)
        commits.emplace_back(std::string(P::to_string(root.kind)), json_string(key.commit));
    }
    if (!commits.empty()) members.emplace_back("root_commits", json_object(commits));
    return with_members(item.detail_json, members);
  }

  Result<void> write_analysis(const AnalysisItem& item, Staged& staged, Memberships& memberships, RunStats& stats) {
    auto who = author(item.who);
    if (!who) return fail(who.error());
    const SourceKey& record = item.keys.record;
    const Uuid analysis = item.ingest.analysis;

    P::AnalysisIngest ingest = item.ingest;
    ingest.changeset = collection_changeset_id(url_, record.commit, analysis);
    ingest.created = item.who.utc;
    ingest.import_source = *source_;
    ingest.author_user = who->user;
    if (ingest.analyst.empty()) ingest.analyst = who->name;
    for (const auto& root : kRoots) ingest.roots.*root.id = root_id(item, root);

    auto ack = store_.ingest({revision_id(url_, record.commit, record.path), sha256(std::string_view{record.blob_sha}),
                              client_, std::move(ingest)});
    if (!ack) {
      if (!P::is_unknown_catalog_reference(ack.error())) return fail(ack.error());
      // One conflict per file, so every file of the collection is accounted for.
      for (const auto& file : collection_files(item))
        if (auto r = stage_unknown_analysis(file, analysis, ack.error().what, staged, stats); !r) return r;
      return {};
    }
    present_.insert(analysis);
    ++stats.analyses;
    for (const auto& file : collection_files(item))
      if (auto r = supersede(file, staged); !r) return r;

    staged.provenance.push_back(provenance("analysis", analysis, record, item.who, analysis_detail(item)));
    for (const auto& root : kRoots)
      if (const SourceKey key = root_key(item, root); !key.path.empty())
        staged.provenance.push_back(provenance("revision", root_id(item, root), key, item.who));

    return join_repositories(analysis, record, !ack->duplicate, who->user, item.repositories, memberships);
  }

  Result<void> stage_membership(const MembershipItem& item, Staged& staged, Memberships& memberships, RunStats& stats) {
    auto present = analysis_present(item.analysis);
    if (!present) return fail(present.error());
    if (!*present)
      return stage_unknown_analysis(item.key, item.analysis, "membership of an analysis that is not in the store",
                                    staged, stats);
    if (auto r = supersede(item.key, staged); !r) return r;
    auto who = author(item.who);
    if (!who) return fail(who.error());
    staged.provenance.push_back(
        provenance("analysis", item.analysis, item.key, item.who, json_object({{"membership_only", "true"}})));
    return join_repositories(item.analysis, item.key, false, who->user, item.repositories, memberships);
  }

  // Membership is written before the batch transaction, which stores the
  // analysis provenance row. That row is therefore the mark that this source
  // already made the analysis a member: add_repository_members logs a change
  // even when it adds nothing, so it is not repeated on a re-run.
  Result<void> join_repositories(Uuid analysis, const SourceKey& record, bool is_new, Uuid added_by,
                                 const std::vector<std::string>& repositories, Memberships& memberships) {
    if (repositories.empty()) return {};
    if (!is_new) {
      auto recorded = store_.has_provenance(*source_, record.commit, record.path);
      if (!recorded) return fail(recorded.error());
      if (*recorded) return {};
    }
    for (const auto& name : repositories) {
      auto& members = memberships[name];
      if (members.analyses.empty()) members.added_by = added_by;
      members.analyses.push_back(analysis);
    }
    return {};
  }

  Result<void> stage_changeset(const ChangesetItem& item, Staged& staged, RunStats& stats) {
    auto who = author(item.who);
    if (!who) return fail(who.error());
    P::ImportedChangeset changeset{changeset_id(url_, item.commit), item.kind, who->user, item.who.utc, item.message, {}};
    for (const auto& revision : item.revisions) {
      auto subject = resolve(revision.subject, revision.kind);
      if (!subject) return fail(subject.error());
      if (!*subject) {
        if (auto r = stage_unknown_analysis(revision.key, std::get<Uuid>(revision.subject),
                                            "revision of an analysis that is not in the store", staged, stats);
            !r)
          return r;
        continue;
      }
      if (auto r = supersede(revision.key, staged); !r) return r;
      const Uuid id = revision_id(url_, revision.key.commit, revision.key.path);
      changeset.revisions.push_back({id, **subject, revision.kind, revision.payload});
      staged.provenance.push_back(provenance("revision", id, revision.key, item.who));
      ++stats.revisions;
    }
    // A changeset none of whose revisions could be written is not written.
    if (changeset.revisions.empty() && !item.revisions.empty()) return {};
    staged.provenance.push_back(provenance("changeset", changeset.uuid, {item.commit, "", ""}, item.who));
    staged.changesets.push_back(std::move(changeset));
    ++stats.changesets;
    return {};
  }

  // Group and bookmark are ensured by ids derived from the tag, so a run that
  // died anywhere between creating them and recording them creates nothing
  // twice. The provenance row marks a bookmark as done.
  Result<void> write_bookmark(const BookmarkItem& item, Staged& recorded) {
    const Uuid id = bookmark_id(url_, item.name);
    auto stored = store_.provenance_for(id);
    if (!stored) return fail(stored.error());
    if (!stored->empty()) return {};
    std::vector<Uuid> analyses;
    for (const Uuid analysis : item.analyses) {
      auto present = analysis_present(analysis);
      if (!present) return fail(present.error());
      if (*present) analyses.push_back(analysis);
    }
    if (analyses.empty()) return {};  // nothing to capture; count_batch applies the same rule
    auto who = author(item.who);
    if (!who) return fail(who.error());
    const P::Actor actor{who->user, client_};
    auto group = store_.create_group(actor, item.name, analyses, bookmark_group_id(url_, item.name));
    if (!group) return fail(group.error());
    auto bookmark = store_.create_bookmark(
        actor, {item.name, "git tag " + item.name + " at " + item.commit, std::nullopt, *group, id});
    if (!bookmark) return fail(bookmark.error());
    recorded.provenance.push_back(provenance("bookmark", *bookmark, {item.commit, bookmark_path(item), ""}, item.who));
    return {};
  }

  // ------------------------------------------------------------ dry run

  Result<bool> conflict_stored(Uuid conflict) {
    auto known = conflicts();
    if (!known) return fail(known.error());
    return (*known)->contains(conflict);
  }

  // A file is accounted for when it has a provenance row or a conflict row.
  Result<bool> accounted(const SourceKey& key) {
    auto recorded = store_.has_provenance(*source_, key.commit, key.path);
    if (!recorded) return fail(recorded.error());
    if (*recorded) return true;
    return conflict_stored(conflict_id(url_, key.commit, key.path));
  }

  // Counts what write_batch would add. Each row is counted once per run.
  Result<void> count_batch(const ImportBatch& batch, RunStats& stats) {
    for (const auto& item : batch.blobs) {
      const auto sha = P::blob_sha256(item.blob.codec, item.blob.bytes);
      if (!counted_blobs_.insert(sha).second) continue;
      auto blob = store_.load_blob(sha);
      if (!blob) return fail(blob.error());
      if (!*blob) ++stats.would_write;
    }
    for (const auto& item : batch.analyses) {
      ++stats.analyses;
      if (!counted_.insert(item.ingest.analysis).second) continue;
      auto exists = analysis_exists(item.ingest.analysis);
      if (!exists) return fail(exists.error());
      if (*exists) {
        importable_.insert(item.ingest.analysis);
        continue;
      }
      // An analysis the store refused is a recorded conflict, not pending work.
      auto refused = conflict_stored(conflict_id(url_, item.keys.record.commit, item.keys.record.path));
      if (!refused) return fail(refused.error());
      if (*refused) continue;
      importable_.insert(item.ingest.analysis);
      ++stats.would_write;
    }
    for (const auto& item : batch.memberships) {
      auto done = accounted(item.key);
      if (!done) return fail(done.error());
      if (!*done) ++stats.would_write;
    }
    for (const auto& item : batch.changesets) {
      ++stats.changesets;
      int missing = 0;
      for (const auto& revision : item.revisions) {
        ++stats.revisions;
        if (!counted_.insert(revision_id(url_, revision.key.commit, revision.key.path)).second) continue;
        auto done = accounted(revision.key);
        if (!done) return fail(done.error());
        if (!*done) ++missing;
      }
      stats.would_write += missing;
      const Uuid changeset = changeset_id(url_, item.commit);
      if ((missing == 0 && !item.revisions.empty()) || !counted_.insert(changeset).second) continue;
      auto rows = store_.provenance_for(changeset);
      if (!rows) return fail(rows.error());
      if (rows->empty()) ++stats.would_write;
    }
    for (const auto& item : batch.conflicts) {
      const Uuid conflict = conflict_id(url_, item.key.commit, item.key.path);
      auto known = conflicts();
      if (!known) return fail(known.error());
      const auto stored = (*known)->find(conflict);
      if (stored == (*known)->end() || stored->second.resolution == kPending) ++stats.conflicts;
      if (!counted_.insert(conflict).second) continue;
      if (stored == (*known)->end()) ++stats.would_write;
    }
    // As write_bookmark: a tag is imported unless it is recorded already or
    // none of its analyses is, or would be, in the store.
    for (const auto& item : batch.bookmarks) {
      const Uuid id = bookmark_id(url_, item.name);
      if (!counted_.insert(id).second) continue;
      auto stored = store_.provenance_for(id);
      if (!stored) return fail(stored.error());
      if (!stored->empty()) continue;
      bool captures = false;
      for (const Uuid analysis : item.analyses) {
        if (importable_.contains(analysis)) captures = true;
        if (captures) break;
        auto exists = analysis_exists(analysis);
        if (!exists) return fail(exists.error());
        captures = *exists;
      }
      if (captures) ++stats.would_write;
    }
    return {};
  }

  P::IStore& store_;
  Uuid client_;
  WriterConfig config_;
  detail::CatalogResolver catalog_;

  // The open source and its last committed progress.
  std::optional<Uuid> source_;
  std::string url_;  // normalized
  std::optional<std::string> token_;
  int done_ = 0, total_ = 0;
  std::optional<std::string> head_;

  std::map<std::string, Author> authors_;  // by git email
  std::set<Uuid> present_;                 // analyses known to be in the store
  std::optional<std::map<Uuid, KnownConflict>> conflicts_;  // of this source; see conflicts()
  bool catching_up_ = false;                                // a replay that has not reached the stored token
  // Dry run only.
  std::set<Uuid> counted_;
  std::set<Uuid> importable_;  // analyses seen that are stored or would be
  std::set<Sha256Digest> counted_blobs_;
};

BatchWriter::BatchWriter(P::IStore& store, Uuid client, WriterConfig config)
    : impl_(std::make_unique<Impl>(store, client, std::move(config))) {}
BatchWriter::~BatchWriter() = default;

Result<P::ImportSourceInfo> BatchWriter::open(ISourceAdapter& adapter) { return impl_->open(adapter); }

Result<RunStats> BatchWriter::run(ISourceAdapter& adapter, std::optional<int> max_batches,
                                  const std::function<bool()>& keep_going,
                                  const std::function<void(const RunStats&, const ImportBatch&)>& on_batch) {
  return impl_->run(adapter, max_batches, keep_going, on_batch);
}

IImportState& BatchWriter::state() { return *impl_; }

}  // namespace pychron::ingest
