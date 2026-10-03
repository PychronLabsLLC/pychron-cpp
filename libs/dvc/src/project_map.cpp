// The content half of the project repository adapter: the files the walk
// selected, read and turned into batch items (project_import.hpp).

#include <algorithm>
#include <cstddef>

#include "legacy_json.hpp"
#include "project_import.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/ingest/ids.hpp"

namespace pychron::dvc::detail {

namespace ps = pychron::persistence;
using ingest::ImportBatch;
using ps::ConflictKind;
using ps::Uuid;

namespace {

constexpr std::string_view kCollectionTag = "<COLLECTION>";
constexpr std::size_t kRecordsPerFetch = 256;
// A rewritten value larger than this is kept as a reference to the git blob
// of its file, not verbatim (spec section 10, item 18).
constexpr std::size_t kMaxRewriteValueBytes = 64 * 1024;

ingest::SourceKey key_of(const FileRef& ref) { return {ref.commit, ref.path, ref.blob_sha}; }

Json reason(std::string_view text) { return Json{{"reason", std::string(text)}}; }

// JSON text the parsers produced, as a value to nest in a detail object.
Json embedded(const std::string& text) {
  auto parsed = parse_legacy(text);
  return parsed ? std::move(*parsed) : Json(text);
}

std::optional<ps::Kind> revision_kind(FileKind kind) {
  switch (kind) {
    case FileKind::Intercepts: return ps::Kind::Intercepts;
    case FileKind::Baselines: return ps::Kind::Baselines;
    case FileKind::Blanks: return ps::Kind::Blanks;
    case FileKind::IcFactors: return ps::Kind::IcFactors;
    case FileKind::Tags: return ps::Kind::Tags;
    case FileKind::Cosmogenic: return ps::Kind::Cosmogenic;
    default: return std::nullopt;
  }
}

const char* kind_name(FileKind kind) {
  switch (kind) {
    case FileKind::Record: return "record";
    case FileKind::Data: return "data";
    case FileKind::Intercepts: return "intercepts";
    case FileKind::Baselines: return "baselines";
    case FileKind::Blanks: return "blanks";
    case FileKind::IcFactors: return "icfactors";
    case FileKind::Tags: return "tags";
    case FileKind::PeakCenter: return "peakcenter";
    case FileKind::Extraction: return "extraction";
    case FileKind::Monitor: return "monitor";
    case FileKind::Cosmogenic: return "cosmogenic";
    default: return "file";
  }
}

bool is_rewritable(FileKind kind) {
  return kind == FileKind::Record || kind == FileKind::Extraction || kind == FileKind::PeakCenter ||
         kind == FileKind::Monitor;
}

// The top-level keys whose values differ between two versions of a file:
// {"<key>": {"old": ..., "new": ...}}, a side left out where the key is
// absent. `before` null: there was no earlier version, or it cannot be read.
// A value larger than kMaxRewriteValueBytes is replaced by a reference to the
// blob of the file version it is in (`before_blob`, `after_blob`).
Json changed_keys(const Json* before, const Json& after, const std::string& before_blob,
                  const std::string& after_blob) {
  const auto kept = [](const Json& value, const std::string& blob) {
    const std::size_t bytes = dump(value).size();
    if (bytes <= kMaxRewriteValueBytes) return value;
    return Json{{"blob_sha", blob}, {"bytes", bytes}};
  };
  Json changed = Json::object();
  if (!after.is_object() || (before && !before->is_object())) {
    if (before) changed["(document)"]["old"] = kept(*before, before_blob);
    changed["(document)"]["new"] = kept(after, after_blob);
    return changed;
  }
  for (auto it = after.begin(); it != after.end(); ++it) {
    const auto old = before ? before->find(it.key()) : after.end();
    if (before && old != before->end() && *old == it.value()) continue;
    if (before && old != before->end()) changed[it.key()]["old"] = kept(*old, before_blob);
    changed[it.key()]["new"] = kept(it.value(), after_blob);
  }
  if (before)
    for (auto it = before->begin(); it != before->end(); ++it)
      if (!after.contains(it.key())) changed[it.key()]["old"] = kept(it.value(), before_blob);
  return changed;
}

// The collection files of a track other than the record, in a fixed order.
std::vector<SeenFile> collection_files(const Track& track) {
  std::vector<SeenFile> files;
  const auto add = [&](FileKind kind, const std::optional<FileRef>& ref) {
    if (ref) files.push_back({kind, *ref});
  };
  add(FileKind::Data, track.data);
  add(FileKind::Intercepts, track.intercepts);
  add(FileKind::Baselines, track.baselines);
  add(FileKind::Blanks, track.blanks);
  add(FileKind::IcFactors, track.icfactors);
  add(FileKind::Tags, track.tags);
  files.insert(files.end(), track.satellites.begin(), track.satellites.end());
  return files;
}

// A folded track keeps its record (it identifies the analysis); the rest is
// not needed again.
void release(Track& track) {
  track.data.reset();
  track.intercepts.reset();
  track.baselines.reset();
  track.blanks.reset();
  track.icfactors.reset();
  track.tags.reset();
  track.satellites.clear();
  track.satellites.shrink_to_fit();
}

}  // namespace

struct Mapper::Reading {
  struct Record {
    std::optional<ParsedRecord> parsed;
    std::optional<ParsedRecord> now;  // the record at fold time (Collect::record_now), when it can be read
    std::string error;
    Sha256Digest digest{};
  };
  std::map<const Track*, Record> records;
};

struct Mapper::Output {
  ImportBatch& batch;
  std::map<int, ingest::ChangesetItem> changesets;  // by commit index
  std::map<int, std::vector<ingest::FileNote>> rewrites;  // by commit index: record and satellite files rewritten
  // The analyses this batch sends, which the store does not have yet: by
  // uuid, the track that sends each; by run id, the uuid.
  std::map<Uuid, const Track*> owners;
  std::map<std::string, Uuid> runids;

  void conflict(const FileRef& ref, ConflictKind kind, std::optional<Uuid> entity,
                std::optional<Sha256Digest> digest, const Json& detail) {
    batch.conflicts.push_back({key_of(ref), entity, kind, digest, dump(detail)});
  }
};

Mapper::Mapper(const ProjectAdapterConfig& config, std::string url, GitReader& reader, const Walk& walk,
               ingest::IImportState& state)
    : config_(config), url_(std::move(url)), reader_(reader), walk_(walk), state_(state),
      context_{config.lab_time_zone} {}

void Mapper::remember(std::vector<GitCommit> commits) {
  for (auto& item : commits) {
    std::string sha = item.sha;
    commits_.insert_or_assign(std::move(sha), std::move(item));
  }
}

void Mapper::silent(const FileRef& ref, ingest::UnitDisposition disposition, ingest::Evidence evidence) {
  if (ledger_) ledger_->silent.push_back({ref.commit, ref.path, disposition, std::move(evidence)});
}

Result<const GitCommit*> Mapper::commit(const std::string& sha) {
  if (const auto it = commits_.find(sha); it != commits_.end()) return &it->second;
  const std::vector<std::string> one{sha};
  auto fetched = reader_.commits(one);
  if (!fetched) return fail(fetched.error());
  if (fetched->size() != 1) return fail(ErrorKind::Protocol, "git: no commit " + sha);
  return &commits_.insert_or_assign(sha, std::move(fetched->front())).first->second;
}

Result<void> Mapper::fetch(std::vector<std::string> blob_shas) {
  std::sort(blob_shas.begin(), blob_shas.end());
  blob_shas.erase(std::unique(blob_shas.begin(), blob_shas.end()), blob_shas.end());
  return reader_.fetch_blobs(blob_shas);
}

// Parses the record of each track. The views of one fetch do not survive the
// next, so everything needed from the bytes is taken here.
Result<void> Mapper::read_records(const std::vector<Collect>& folds, Reading& reading) {
  for (std::size_t begin = 0; begin < folds.size(); begin += kRecordsPerFetch) {
    const std::size_t end = std::min(folds.size(), begin + kRecordsPerFetch);
    std::vector<std::string> blobs;
    for (std::size_t i = begin; i < end; ++i) {
      blobs.push_back(folds[i].track->record->blob_sha);
      if (folds[i].record_now) blobs.push_back(folds[i].record_now->blob_sha);
    }
    if (auto r = fetch(std::move(blobs)); !r) return r;
    for (std::size_t i = begin; i < end; ++i) {
      auto text = reader_.blob(folds[i].track->record->blob_sha);
      if (!text) return fail(text.error());
      Reading::Record read;
      read.digest = sha256(*text);
      auto parsed = parse_record(*text, context_);
      if (parsed)
        read.parsed = std::move(*parsed);
      else
        read.error = parsed.error().what;
      if (folds[i].record_now) {
        auto text_now = reader_.blob(folds[i].record_now->blob_sha);
        if (!text_now) return fail(text_now.error());
        if (auto parsed_now = parse_record(*text_now, context_)) read.now = std::move(*parsed_now);
      }
      reading.records.insert_or_assign(folds[i].track, std::move(read));
    }
  }
  return {};
}

// Tracks an earlier run folded: who the analysis is, and whether it is this
// source's. An analysis that is not in the store was refused then (a missing
// catalog row); its revisions are still sent, and the writer records them as
// refused until a replay imports it.
Result<void> Mapper::resolve(const std::vector<Track*>& tracks) {
  Reading reading;
  std::vector<Collect> folds;
  for (Track* track : tracks) folds.push_back({track, std::nullopt});
  if (auto r = read_records(folds, reading); !r) return r;
  for (Track* track : tracks) {
    const auto& read = reading.records.at(track);
    if (!read.parsed) {
      track->role = Track::Role::Broken;
      continue;
    }
    track->uuid =
        read.parsed->had_uuid ? read.parsed->ingest.analysis : ingest::derived_analysis_id(url_, read.parsed->runid);
    auto origin = state_.analysis_origin(track->uuid, track->record->commit);
    if (!origin) return fail(origin.error());
    track->role = !*origin || (*origin)->from_this_source ? Track::Role::Imported : Track::Role::Foreign;
  }
  return {};
}

Result<void> Mapper::map(const std::vector<Work>& work, ImportBatch& batch) {
  Output out{batch, {}, {}, {}, {}};

  // The records first: they say which analysis each file belongs to.
  std::vector<Collect> collects;
  std::vector<Track*> unresolved;
  for (const auto& item : work)
    if (const auto* fold = std::get_if<Collect>(&item)) collects.push_back(*fold);
  for (const auto& item : work) {
    const auto* file = std::get_if<Change>(&item);
    if (!file || !file->track || !file->track->flushed || file->track->role != Track::Role::Unresolved) continue;
    const bool folded = std::any_of(collects.begin(), collects.end(),
                                    [&](const Collect& fold) { return fold.track == file->track; });
    if (!folded && std::find(unresolved.begin(), unresolved.end(), file->track) == unresolved.end())
      unresolved.push_back(file->track);
  }
  Reading reading;
  if (auto r = read_records(collects, reading); !r) return r;
  if (auto r = resolve(unresolved); !r) return r;

  // Then every other file of the batch, in one read.
  std::vector<std::string> blobs;
  for (const auto& fold : collects) {
    for (const auto& file : collection_files(*fold.track)) blobs.push_back(file.ref.blob_sha);
    const auto& read = reading.records.at(fold.track);
    if (read.parsed && read.parsed->spec_sha && !snapshots_.contains(*read.parsed->spec_sha))
      if (const FileRef* settings = walk_.spectrometer(*read.parsed->spec_sha)) blobs.push_back(settings->blob_sha);
  }
  for (const auto& item : work)
    if (const auto* file = std::get_if<Change>(&item); file && !(file->track && !file->track->flushed)) {
      blobs.push_back(file->ref.blob_sha);
      if (file->previous) blobs.push_back(file->previous->blob_sha);  // to say what a rewrite changed
    }
  if (auto r = fetch(std::move(blobs)); !r) return r;

  for (const auto& item : work) {
    if (const auto* fold = std::get_if<Collect>(&item)) {
      if (auto r = collect(*fold, reading, out); !r) return r;
    } else if (auto r = change(std::get<Change>(item), out); !r) {
      return r;
    }
  }

  for (auto& [index, changeset] : out.changesets) {
    // What the commit rewrote without a revision is kept with its changeset.
    if (const auto rewritten_here = out.rewrites.find(index); rewritten_here != out.rewrites.end())
      changeset.rewrites = std::move(rewritten_here->second);
    // A commit that only touches reference data is a reference changeset.
    const bool reference =
        !changeset.revisions.empty() &&
        std::all_of(changeset.revisions.begin(), changeset.revisions.end(),
                    [](const ingest::RevisionItem& revision) { return revision.kind == ps::Kind::RefValue; });
    if (reference) changeset.kind = ps::ChangesetKind::Reference;
    batch.changesets.push_back(std::move(changeset));
  }
  return {};
}

Result<void> Mapper::bookmark(const GitTag& tag, ImportBatch& batch) {
  std::vector<Track*> unresolved;
  for (Track* track : walk_.flushed())
    if (track->role == Track::Role::Unresolved) unresolved.push_back(track);
  if (auto r = resolve(unresolved); !r) return r;
  auto meta = commit(tag.commit);
  if (!meta) return fail(meta.error());
  ingest::BookmarkItem item;
  item.name = tag.name;
  item.commit = tag.commit;
  item.who = (*meta)->author;
  for (const Track* track : walk_.flushed())
    if (track->role == Track::Role::Imported) item.analyses.push_back(track->uuid);
  batch.bookmarks.push_back(std::move(item));
  return {};
}

// ---------------------------------------------------------------- collections

Result<void> Mapper::collect(const Collect& fold, Reading& reading, Output& out) {
  Track& track = *fold.track;
  const FileRef record_ref = *track.record;
  auto& read = reading.records.at(&track);
  const std::vector<SeenFile> files = collection_files(track);
  // The other files of a collection that is not imported, one conflict each,
  // so every file is accounted for.
  const auto refuse_files = [&](std::optional<Uuid> entity, const std::string& why) {
    for (const auto& file : files)
      out.conflict(file.ref, ConflictKind::UnknownAnalysis, entity, std::nullopt, reason(why));
  };

  if (!read.parsed) {
    out.conflict(record_ref, ConflictKind::Unparseable, std::nullopt, read.digest, reason(read.error));
    refuse_files(std::nullopt, "the analysis record " + record_ref.path + " cannot be read");
    track.role = Track::Role::Broken;
    release(track);
    return {};
  }
  ParsedRecord& record = *read.parsed;
  const Uuid uuid = record.had_uuid ? record.ingest.analysis : ingest::derived_analysis_id(url_, record.runid);
  track.uuid = uuid;

  auto meta = commit(record_ref.commit);
  if (!meta) return fail(meta.error());
  const ingest::GitWho who = (*meta)->author;
  const bool collection_commit = std::string_view((*meta)->message).starts_with(kCollectionTag);

  // The record was rewritten while the analysis was pending: it is imported
  // under the run identity it has now, when the collection is folded. (A
  // rewrite that carries another uuid is a conflict, reported with that file.)
  if (read.now) {
    const Uuid uuid_now = read.now->had_uuid ? read.now->ingest.analysis
                                             : ingest::derived_analysis_id(url_, read.now->runid);
    if (uuid_now == uuid) {
      record.ingest.identifier = read.now->ingest.identifier;
      record.ingest.aliquot = read.now->ingest.aliquot;
      record.ingest.increment = read.now->ingest.increment;
      record.runid = read.now->runid;
    }
  }

  // Another copy of the same analysis. In the store from another source: it
  // joins this repository and nothing else. From this source, under another
  // path (in the store, or earlier in this batch): it is a member already.
  // Either way a copy whose record differs from the imported one is reported.
  auto origin = state_.analysis_origin(uuid, record_ref.commit);
  if (!origin) return fail(origin.error());
  const auto batch_owner = out.owners.find(uuid);
  const bool copy_in_batch = !*origin && batch_owner != out.owners.end() && batch_owner->second != &track;
  if ((*origin && !(*origin)->from_this_source) || copy_in_batch) {
    const bool same_source = copy_in_batch || (*origin)->in_this_source;
    if (!same_source) out.batch.memberships.push_back({uuid, key_of(record_ref), who, {config_.repository_name}});
    const std::string imported = copy_in_batch ? batch_owner->second->record->blob_sha : (*origin)->record_blob_sha;
    if (!imported.empty() && imported != record_ref.blob_sha) {
      Json detail = reason("the record differs from the one this analysis was imported from");
      detail["imported_blob"] = imported;
      detail["blob"] = record_ref.blob_sha;
      out.conflict(record_ref, ConflictKind::IdentityClash, uuid, read.digest, detail);
    }
    // None of its other files has a row: the analysis is the copy imported
    // under another path (same source) or the membership row at this record.
    ingest::Evidence host{ingest::Evidence::Kind::Recorded, record_ref.commit, record_ref.path};
    if (same_source) {
      host = {ingest::Evidence::Kind::Entity, {}, {}, {}, uuid};
      silent(record_ref, ingest::UnitDisposition::Folded, host);
    }
    for (const auto& file : files) silent(file.ref, ingest::UnitDisposition::Folded, host);
    track.role = Track::Role::Foreign;
    release(track);
    return {};
  }

  // Two analyses cannot share a run id: not in this batch, not with one an
  // earlier run or another source imported. An analysis this source already
  // imported is sent again as it is: its run id may have changed since.
  if (!*origin) {
    std::optional<Uuid> holder;
    if (const auto taken = out.runids.find(record.runid); taken != out.runids.end()) {
      holder = taken->second;
    } else {
      auto stored =
          state_.analysis_with_runid(record.ingest.identifier, record.ingest.aliquot, record.ingest.increment);
      if (!stored) return fail(stored.error());
      holder = *stored;
    }
    if (holder && *holder != uuid) {
      Json detail = reason("run id " + record.runid + " belongs to another analysis");
      detail["uuid"] = uuid.str();
      detail["imported_uuid"] = holder->str();
      out.conflict(record_ref, ConflictKind::IdentityClash, *holder, read.digest, detail);
      refuse_files(uuid, "the analysis " + record.runid + " was not imported: its run id is taken");
      track.role = Track::Role::Broken;
      release(track);
      return {};
    }
  }

  ingest::AnalysisItem item;
  item.ingest = std::move(record.ingest);
  item.ingest.analysis = uuid;
  item.keys.record = key_of(record_ref);
  item.who = who;
  item.repositories = {config_.repository_name};
  auto& roots = item.ingest.roots;

  Json detail = Json::object();
  detail["runid"] = record.runid;
  if (!record.had_uuid) detail["derived_uuid"] = true;
  if (fold.record_now && read.now) detail["identity_from"] = fold.record_now->commit;
  if (!record.notes.empty()) detail["notes"] = record.notes;
  std::vector<std::pair<int, std::string>> commits{{record_ref.index, record_ref.commit}};
  Json unparseable = Json::array();
  // An analysis this source already created is sent again as it is (a replay,
  // or a run that died before its batch was recorded). A root file that has
  // no provenance yet was never written: it is also sent as a revision, which
  // the store skips when the root exists under the same id.
  const bool resent = origin->has_value();
  struct Unrecorded {
    FileRef ref;
    ps::Kind kind;
    ps::RevisionPayload payload;
  };
  std::vector<Unrecorded> unrecorded;
  const auto check_recorded = [&](const FileRef& ref, ps::Kind kind, ps::RevisionPayload payload) -> Result<void> {
    if (!resent) return {};
    auto recorded = state_.imported(ref.commit, ref.path);
    if (!recorded) return fail(recorded.error());
    if (!*recorded) unrecorded.push_back({ref, kind, std::move(payload)});
    return {};
  };

  for (const auto& file : files) {
    auto text = reader_.blob(file.ref.blob_sha);
    if (!text) return fail(text.error());
    const auto bad = [&](const Error& error) {
      out.conflict(file.ref, ConflictKind::Unparseable, uuid, sha256(*text), reason(error.what));
      unparseable.push_back(file.ref.path);
    };
    const auto extra = [&](const std::optional<std::string>& json) {
      if (json) detail[std::string(kind_name(file.kind)) + "_extra"] = embedded(*json);
    };
    commits.emplace_back(file.ref.index, file.ref.commit);
    switch (file.kind) {
      case FileKind::Data: {
        auto data = parse_data(*text);
        if (!data) {
          bad(data.error());
          break;
        }
        roots.signal_refs = std::move(data->refs);
        for (auto& blob : data->blobs) out.batch.blobs.push_back({key_of(file.ref), std::move(blob)});
        item.keys.signals = key_of(file.ref);
        extra(data->extra_json);
        if (auto r = check_recorded(file.ref, ps::Kind::Signals, roots.signal_refs); !r) return r;
        break;
      }
      case FileKind::Extraction:
      case FileKind::PeakCenter:
      case FileKind::Monitor: {
        std::vector<ps::BlobIngest> scans;
        ps::AnalysisIngest merged = item.ingest;  // a file that fails leaves nothing behind
        if (auto ok = merge_satellite(file.kind, *text, merged, scans); !ok) {
          bad(ok.error());
          break;
        }
        item.ingest = std::move(merged);
        for (auto& blob : scans) out.batch.blobs.push_back({key_of(file.ref), std::move(blob)});
        // Folded into the analysis: its row is the record's.
        silent(file.ref, ingest::UnitDisposition::Folded,
               {ingest::Evidence::Kind::Recorded, record_ref.commit, record_ref.path});
        break;
      }
      default: {
        auto revision = parse_revision(file.kind, *text);
        if (!revision) {
          bad(revision.error());
          break;
        }
        extra(revision->extra_json);
        if (auto r = check_recorded(file.ref, *revision_kind(file.kind), revision->payload); !r) return r;
        if (file.kind == FileKind::Intercepts) {
          roots.intercepts_rows = std::get<ps::Intercepts>(std::move(revision->payload));
          item.keys.intercepts = key_of(file.ref);
        } else if (file.kind == FileKind::Baselines) {
          roots.baselines_rows = std::get<ps::Baselines>(std::move(revision->payload));
          item.keys.baselines = key_of(file.ref);
        } else if (file.kind == FileKind::Blanks) {
          roots.blanks_rows = std::get<ps::Blanks>(std::move(revision->payload));
          item.keys.blanks = key_of(file.ref);
        } else if (file.kind == FileKind::IcFactors) {
          roots.icfactors_rows = std::get<ps::IcFactors>(std::move(revision->payload));
          item.keys.icfactors = key_of(file.ref);
        } else {
          roots.tag = std::get<ps::TagValue>(std::move(revision->payload));
          item.keys.tags = key_of(file.ref);
        }
      }
    }
  }

  // No tags file: later legacy versions kept the tag only in the database.
  if (item.keys.tags.path.empty() && config_.tag_lookup) {
    if (auto name = config_.tag_lookup(uuid); name && !name->empty()) {
      roots.tag.name = std::move(*name);
      detail["tag_from_db"] = true;
    }
  }

  if (record.spec_sha) {
    auto settings = snapshot(*record.spec_sha);
    if (!settings) return fail(settings.error());
    if (*settings) {
      item.ingest.spectrometer_snapshot = std::move(**settings);
      // The settings file becomes the snapshot of the analyses that name it.
      if (const FileRef* file = walk_.spectrometer(*record.spec_sha))
        silent(*file, ingest::UnitDisposition::Folded,
               {ingest::Evidence::Kind::Recorded, record_ref.commit, record_ref.path});
    } else {
      detail["spectrometer_file_unavailable"] = *record.spec_sha;  // not in the repository, or unreadable
    }
  }

  // A collection whose record did not arrive in a <COLLECTION> commit, or that
  // never became complete, is one this importer put together.
  item.synthetic_collection = !collection_commit || !track.complete();
  std::sort(commits.begin(), commits.end());
  Json shas = Json::array();
  for (const auto& [index, sha] : commits)
    if (shas.empty() || shas.back() != sha) shas.push_back(sha);
  detail["collection_commits"] = std::move(shas);
  if (!unparseable.empty()) detail["unparseable"] = std::move(unparseable);
  item.detail_json = dump(detail);

  if (config_.catalog_from_repos)
    if (auto r = synthesize_catalog(record, item.ingest, record_ref, out); !r) return r;
  out.batch.analyses.push_back(std::move(item));
  for (auto& late : unrecorded)
    if (auto r = add_revision(late.ref, uuid, late.kind, std::move(late.payload), "{}", out); !r) return r;

  out.owners.insert_or_assign(uuid, &track);
  out.runids.insert_or_assign(record.runid, uuid);
  track.role = Track::Role::Imported;
  release(track);
  return {};
}

Result<std::optional<ps::SpectrometerSnapshot>> Mapper::snapshot(const std::string& sha1) {
  if (const auto cached = snapshots_.find(sha1); cached != snapshots_.end()) return cached->second;
  const FileRef* file = walk_.spectrometer(sha1);
  if (!file) return std::optional<ps::SpectrometerSnapshot>{};  // not in the repository (yet)
  auto text = reader_.blob(file->blob_sha);
  if (!text) return fail(text.error());
  // A file that does not parse is reported where it was added (change()).
  auto parsed = parse_spectrometer(*text, sha1);
  auto& slot = snapshots_[sha1];
  if (parsed) slot = std::move(*parsed);
  return slot;
}

// The catalog rows a record implies, for a source with no catalog dump. The
// identifier is the row an analysis cannot be imported without; making one up
// is recorded as a conflict so that it is seen. The conflict is keyed by the
// identifier, not by a file, so there is one per identifier however often the
// import is resumed.
Result<void> Mapper::synthesize_catalog(const ParsedRecord& record, const ps::AnalysisIngest& analysis,
                                        const FileRef& from, Output& out) {
  const auto once = [&](std::string what) { return sent_.insert(std::move(what)).second; };
  auto& catalog = out.batch.catalog;
  if (once("mass_spectrometer\n" + analysis.mass_spectrometer)) {
    ps::MassSpectrometerSpec spec;
    spec.name = analysis.mass_spectrometer;
    catalog.push_back(ingest::MassSpecItem{std::move(spec)});
  }
  if (analysis.extract_device && once("extract_device\n" + *analysis.extract_device))
    catalog.push_back(ingest::ExtractDeviceItem{*analysis.extract_device});
  if (!once("identifier\n" + analysis.identifier)) return {};

  const auto& names = record.catalog;
  Json detail{{"synthesized", true}, {"table", "identifier"}, {"identifier", analysis.identifier}};
  detail["from"] = Json{{"commit", from.commit}, {"path", from.path}};
  bool placed = false;
  const bool irradiated = names.irradiation && names.irradiation_level && names.irradiation_position;
  if (analysis.analysis_type == "unknown" && irradiated) {
    const std::string where =
        *names.irradiation + "\n" + *names.irradiation_level + "\n" + std::to_string(*names.irradiation_position);
    // One identifier per irradiation position: a second one there gets none,
    // whether the first came in this walk, an earlier run or another source.
    std::string holder;
    if (const auto here = positions_.find(where); here != positions_.end()) {
      holder = here->second;
    } else {
      auto stored =
          state_.identifier_at(*names.irradiation, *names.irradiation_level, *names.irradiation_position);
      if (!stored) return fail(stored.error());
      holder = stored->value_or(analysis.identifier);
      positions_.emplace(where, holder);
    }
    if (holder == analysis.identifier) {
      ingest::PositionItem position;
      position.irradiation = *names.irradiation;
      position.level = *names.irradiation_level;
      position.position = *names.irradiation_position;
      position.identifier = analysis.identifier;
      if (names.sample && names.project && names.material) {
        position.sample = names.sample;
        position.project = names.project;
        position.material = names.material;
      }
      catalog.push_back(std::move(position));
      detail["irradiation"] = *names.irradiation;
      detail["level"] = *names.irradiation_level;
      detail["position"] = *names.irradiation_position;
      placed = true;
    } else {
      detail["position_taken_by"] = holder;
    }
  }
  if (!placed) {
    ingest::SpecialIdentifierItem special;
    special.identifier = analysis.identifier;
    special.analysis_type = analysis.analysis_type;
    special.mass_spectrometer = analysis.mass_spectrometer;
    catalog.push_back(std::move(special));
    detail["special"] = true;
  }
  out.batch.conflicts.push_back({{"", "catalog/identifier/" + analysis.identifier, ""},
                                 std::nullopt,
                                 ConflictKind::IdentityClash,
                                 std::nullopt,
                                 dump(detail)});
  return {};
}

// ---------------------------------------------------------------- later changes

// Whether a revision file holds what its path was last imported with: it was
// removed and is back unchanged. The walk says so (Change::restored). The
// store is not asked: in a replay its head is the outcome of later commits,
// and a file restored before them would be taken for a change and written
// over the head.
bool Mapper::unchanged(const Change& item) {
  if (item.restored)
    silent(item.ref, ingest::UnitDisposition::Unchanged,
           {ingest::Evidence::Kind::Blob, {}, item.ref.path, item.ref.blob_sha});
  return item.restored;
}

// The changeset of the commit `ref` belongs to, made when first asked for.
Result<ingest::ChangesetItem*> Mapper::changeset_of(const FileRef& ref, Output& out) {
  auto found = out.changesets.find(ref.index);
  if (found == out.changesets.end()) {
    auto meta = commit(ref.commit);
    if (!meta) return fail(meta.error());
    ingest::ChangesetItem changeset;
    changeset.commit = ref.commit;
    changeset.who = (*meta)->author;
    changeset.message = (*meta)->message;
    found = out.changesets.emplace(ref.index, std::move(changeset)).first;
  }
  return &found->second;
}

Result<void> Mapper::add_revision(const FileRef& ref, ingest::SubjectRef subject, ps::Kind kind,
                                  ps::RevisionPayload payload, std::string detail_json, Output& out,
                                  std::string identifier) {
  auto changeset = changeset_of(ref, out);
  if (!changeset) return fail(changeset.error());
  (*changeset)->revisions.push_back({key_of(ref), std::move(subject), kind, std::move(payload),
                                     std::move(detail_json), std::move(identifier)});
  return {};
}

Result<void> Mapper::change(const Change& item, Output& out) {
  const FileRef& ref = item.ref;
  if (item.track && !item.track->flushed) {
    out.conflict(ref, ConflictKind::UnknownAnalysis, std::nullopt, std::nullopt,
                 reason("no analysis record for " + item.track->key + " in this repository"));
    return {};
  }
  auto text = reader_.blob(ref.blob_sha);
  if (!text) return fail(text.error());
  const auto bad = [&](const std::string& why) {
    out.conflict(ref, ConflictKind::Unparseable, std::nullopt, sha256(*text), reason(why));
  };

  switch (item.info.kind) {
    case FileKind::Unknown:
      bad("not a file of a legacy repository");
      return {};

    case FileKind::Spectrometer: {
      auto parsed = parse_spectrometer(*text, item.info.key);
      auto& slot = snapshots_[item.info.key];
      slot.reset();
      if (parsed)
        slot = std::move(*parsed);
      else
        bad(parsed.error().what);
      return {};
    }

    case FileKind::FrozenProduction: {
      const auto [irradiation, level] = split_frozen_production_key(item.info.key);
      const std::string name = "frozen/" + config_.repository_name + "/" + irradiation + "/" + level;
      const ingest::SubjectRef subject =
          ingest::RefObjectKey{std::string(ps::to_string(ps::RefType::Production)), name};
      if (unchanged(item)) return {};
      auto parsed = parse_frozen_production(*text, irradiation, level);
      if (!parsed) {
        bad(parsed.error().what);
        return {};
      }
      // Not scoped to its level: nothing resolves a frozen production by scope.
      ingest::RefObjectItem object;
      object.type = ps::RefType::Production;
      object.key = name;
      out.batch.catalog.push_back(std::move(object));
      Json detail = Json::object();
      if (parsed->name) detail["name"] = *parsed->name;
      if (parsed->extra_json) detail["extra"] = embedded(*parsed->extra_json);
      return add_revision(ref, subject, ps::Kind::RefValue, ps::RefPayload{std::move(parsed->value)}, dump(detail),
                          out);
    }

    case FileKind::InterpretedAge: {
      const ingest::SubjectRef subject = ingest::InterpretedAgeKey{ref.path};
      if (unchanged(item)) return {};
      auto parsed = parse_interpreted_age(*text, item.info.key);
      if (!parsed) {
        bad(parsed.error().what);
        return {};
      }
      ingest::InterpretedAgeItem age;
      age.key = ref.path;
      age.name = parsed->name;
      if (!parsed->identifier.empty()) age.identifier = parsed->identifier;
      age.repository = config_.repository_name;
      out.batch.catalog.push_back(std::move(age));
      Json detail = Json::object();
      detail["format"] = parsed->nested ? "nested" : "flat";
      if (parsed->uuid) detail["legacy_uuid"] = parsed->uuid->str();
      if (!parsed->notes.empty()) detail["notes"] = parsed->notes;
      return add_revision(ref, subject, ps::Kind::InterpretedAge, std::move(parsed->value), dump(detail), out);
    }

    default:
      return analysis_change(item, *text, out);
  }
}

// A file of an analysis whose collection is already folded.
Result<void> Mapper::analysis_change(const Change& item, std::string_view text, Output& out) {
  const FileRef& ref = item.ref;
  const Track& track = *item.track;
  const FileKind kind = item.info.kind;
  const Uuid uuid = track.uuid;

  if (track.role == Track::Role::Broken) {
    out.conflict(ref, ConflictKind::UnknownAnalysis, std::nullopt, std::nullopt,
                 reason("the analysis of " + track.record->path + " was not imported"));
    return {};
  }
  if (track.role == Track::Role::Foreign) {
    Json detail = reason("the analysis was imported from another source; this change is not applied");
    out.conflict(ref, ConflictKind::IdentityClash, uuid, sha256(text), detail);
    return {};
  }

  if (is_rewritable(kind)) return rewritten(item, text, out);

  const ps::Kind store_kind = kind == FileKind::Data ? ps::Kind::Signals : *revision_kind(kind);
  if (unchanged(item)) return {};

  if (kind == FileKind::Data) {
    auto data = parse_data(text);
    if (!data) {
      out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(data.error().what));
      return {};
    }
    for (auto& blob : data->blobs) out.batch.blobs.push_back({key_of(ref), std::move(blob)});
    Json detail = Json::object();
    if (data->extra_json) detail["extra"] = embedded(*data->extra_json);
    return add_revision(ref, uuid, store_kind, std::move(data->refs), dump(detail), out);
  }

  auto revision = parse_revision(kind, text);
  if (!revision) {
    out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(revision.error().what));
    return {};
  }
  Json detail = Json::object();
  if (revision->extra_json) detail["extra"] = embedded(*revision->extra_json);
  return add_revision(ref, uuid, store_kind, std::move(revision->payload), dump(detail), out);
}

// A record or satellite file of an imported analysis written again (legacy
// <SYNC>, <EDIT>, <DEFINE EQUIL> and manual commits). These files are not
// revisioned. A record that changes the run identity yields an identity
// revision; whatever else changed is kept, old and new, in the provenance of
// the commit's changeset. A record that now carries another uuid is a
// different analysis in the same file: that is a conflict.
Result<void> Mapper::rewritten(const Change& item, std::string_view text, Output& out) {
  const FileRef& ref = item.ref;
  const FileKind kind = item.info.kind;
  const Uuid uuid = item.track->uuid;
  // Removed and added again with what it had: nothing was rewritten.
  if (item.previous && item.previous->blob_sha == ref.blob_sha) {
    silent(ref, ingest::UnitDisposition::Unchanged,
           {ingest::Evidence::Kind::Recorded, item.track->record->commit, item.track->record->path});
    return {};
  }

  auto after = parse_legacy(text);
  if (!after) {
    out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(after.error().what));
    return {};
  }
  std::optional<Json> before;
  std::optional<ParsedRecord> record_before;
  if (item.previous) {
    auto old_text = reader_.blob(item.previous->blob_sha);
    if (!old_text) return fail(old_text.error());
    if (auto parsed = parse_legacy(*old_text)) before = std::move(*parsed);
    if (kind == FileKind::Record)
      if (auto parsed = parse_record(*old_text, context_)) record_before = std::move(*parsed);
  }

  if (kind == FileKind::Record) {
    auto record = parse_record(text, context_);
    if (!record) {
      out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(record.error().what));
      return {};
    }
    const Uuid now = record->had_uuid ? record->ingest.analysis : ingest::derived_analysis_id(url_, record->runid);
    if (now != uuid) {
      Json detail = reason("the record now carries another uuid; the imported analysis is unchanged");
      detail["imported_uuid"] = uuid.str();
      detail["uuid"] = now.str();
      out.conflict(ref, ConflictKind::IdentityClash, uuid, sha256(text), detail);
      return {};
    }
    // A rewrite held with a pending collection needs no revision: the
    // analysis was folded under the identity its record had by then. Without
    // a readable earlier version the stored identity is not known here; the
    // revision is then sent, and changes nothing if it is the same.
    if (!item.held && (!record_before || record_before->runid != record->runid)) {
      // No catalog: the identifier a record is renumbered to is made as the
      // identifier of a collected record is.
      if (config_.catalog_from_repos)
        if (auto r = synthesize_catalog(*record, record->ingest, ref, out); !r) return r;
      ps::IdentityValue identity;
      identity.aliquot = record->ingest.aliquot;
      identity.increment = record->ingest.increment;
      identity.reason = "legacy record rewritten";
      if (auto r = add_revision(ref, uuid, ps::Kind::Identity, identity, "{}", out, record->ingest.identifier); !r)
        return r;
    }
  }

  auto changeset = changeset_of(ref, out);  // exists even when the commit has no revision
  if (!changeset) return fail(changeset.error());
  Json entry = Json::object();
  entry["path"] = ref.path;
  entry["blob"] = ref.blob_sha;
  entry["kind"] = kind_name(kind);
  entry["analysis"] = uuid.str();
  entry["changed"] = changed_keys(before ? &*before : nullptr, *after,
                                  item.previous ? item.previous->blob_sha : std::string(), ref.blob_sha);
  out.rewrites[ref.index].push_back({ref.path, dump(entry)});
  return {};
}

}  // namespace pychron::dvc::detail
