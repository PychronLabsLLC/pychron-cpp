// The content half of the project repository adapter: the files the walk
// selected, read and turned into batch items (project_import.hpp).

#include <algorithm>
#include <cstddef>
#include <new>

#include "legacy_json.hpp"
#include "project_import.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/ingest/conflict_markers.hpp"
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
  // The versions this batch found readable, and the deletions it was handed:
  // each ends the wait of the unreadable versions before it (spec 10.37).
  // Looked at once every file of the batch is mapped (supersede_before reads
  // blobs, which ends the life of the views the mapping holds).
  std::vector<FileRef> settled;
  // The files this batch refused because the record of their analysis cannot
  // be read, by track: what Mapper::recover puts together again when a
  // readable record follows in the same batch.
  std::map<const Track*, std::vector<SeenFile>> refused;

  void conflict(const FileRef& ref, ConflictKind kind, std::optional<Uuid> entity,
                std::optional<Sha256Digest> digest, const Json& detail) {
    batch.conflicts.push_back({key_of(ref), entity, kind, digest, dump(detail)});
  }
};

Mapper::Mapper(const ProjectAdapterConfig& config, std::string url, GitReader& reader, Walk& walk,
               const std::vector<std::string>& order, ingest::IImportState& state)
    : config_(config), url_(std::move(url)), reader_(reader), walk_(walk), order_(order), state_(state),
      context_{config.lab_time_zone} {}

// ---------------------------------------------------------------- unreadable versions (spec 10.37)

void Mapper::judge(const FileRef& ref, bool readable) {
  auto* versions = walk_.versions(ref.path);
  if (!versions) return;
  for (auto version = versions->rbegin(); version != versions->rend(); ++version)
    if (version->index == ref.index) {
      version->read = readable ? Version::Read::Yes : Version::Read::No;
      return;
    }
}

// Whether a file of this kind can be read at all: every check the mapping
// applies to a version of it, wherever in the life of its analysis the
// version falls. A version this accepts was accepted when it was mapped.
Result<void> Mapper::check_readable(const PathInfo& info, std::string_view text) const {
  const auto said = [](const auto& parsed) -> Result<void> {
    if (!parsed) return fail(parsed.error());
    return {};
  };
  switch (info.kind) {
    case FileKind::Record:
      return said(parse_record(text, context_));
    case FileKind::Data:
      return said(parse_data(text));
    case FileKind::Extraction:
    case FileKind::PeakCenter:
    case FileKind::Monitor: {
      if (auto json = parse_legacy(text); !json) return fail(json.error());
      ps::AnalysisIngest scratch;
      std::vector<ps::BlobIngest> scans;
      return merge_satellite(info.kind, text, scratch, scans);
    }
    case FileKind::Intercepts:
    case FileKind::Baselines:
    case FileKind::Blanks:
    case FileKind::IcFactors:
    case FileKind::Tags:
    case FileKind::Cosmogenic:
      return said(parse_revision(info.kind, text));
    case FileKind::InterpretedAge:
      return said(parse_interpreted_age(text, info.key));
    case FileKind::FrozenProduction: {
      const auto [irradiation, level] = split_frozen_production_key(info.key);
      return said(parse_frozen_production(text, irradiation, level));
    }
    case FileKind::Spectrometer:
      return said(parse_spectrometer(text, info.key));
    default:
      return {};  // not a file the import reads
  }
}

Result<std::string_view> Mapper::blob_text(const std::string& blob_sha) {
  if (auto text = reader_.blob(blob_sha)) return text;
  const std::vector<std::string> one{blob_sha};
  if (auto r = reader_.fetch_blobs(one); !r) return fail(r.error());
  return reader_.blob(blob_sha);
}

// `ref` is a readable version of its path, or its deletion. The versions
// before it that could not be read, back to the last readable one or
// deletion, no longer wait for a good one: their conflicts are listed as
// superseded. A version an earlier run mapped is read again here; what it is
// depends on its bytes alone, so the list is the same in every walk of the
// history. A version that had no conflict of its own (it was never mapped,
// or was refused for another reason) may be listed too: the writer passes
// over a key that has no conflict a later version answers.
Result<void> Mapper::supersede_before(const FileRef& ref, ImportBatch& batch) {
  auto* versions = walk_.versions(ref.path);
  if (!versions) return {};
  std::size_t at = versions->size();
  while (at > 0 && (*versions)[at - 1].index != ref.index) --at;
  if (at == 0) return {};
  const PathInfo info = classify_path(ref.path);
  for (std::size_t v = at - 1; v-- > 0;) {
    Version& earlier = (*versions)[v];
    if (earlier.blob_sha.empty()) break;  // a deletion: it settled what came before it
    if (earlier.read == Version::Read::Unknown) {
      auto text = blob_text(earlier.blob_sha);
      if (!text) return fail(text.error());
      earlier.read = check_readable(info, *text) ? Version::Read::Yes : Version::Read::No;
    }
    if (earlier.read == Version::Read::Yes) break;
    batch.superseded.push_back({order_[static_cast<std::size_t>(earlier.index)], ref.path, earlier.blob_sha});
  }
  return {};
}

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
    auto& read = reading.records.at(track);
    if (!read.parsed) {
      // The record it was folded with cannot be read. An earlier run started
      // the collection at the first readable version of the record, when one
      // came before this batch (recover()).
      if (auto* versions = walk_.versions(track->record->path)) {
        bool after_first = false;
        for (auto& version : *versions) {
          if (version.index >= batch_first_) break;
          if (!after_first) {
            after_first = version.index == track->record->index;
            continue;
          }
          if (version.blob_sha.empty() || version.read == Version::Read::No) continue;
          auto text = blob_text(version.blob_sha);
          if (!text) return fail(text.error());
          auto parsed = parse_record(*text, context_);
          version.read = parsed ? Version::Read::Yes : Version::Read::No;
          if (!parsed) continue;
          read.parsed = std::move(*parsed);
          track->record = FileRef{version.index, order_[static_cast<std::size_t>(version.index)], track->record->path,
                                  version.blob_sha};
          track->folded_at = version.index;
          track->recovered = true;
          break;
        }
      }
    }
    if (!read.parsed) {
      track->role = Track::Role::Broken;
      track->record_unreadable = true;
      continue;
    }
    track->uuid =
        read.parsed->had_uuid ? read.parsed->ingest.analysis : ingest::derived_analysis_id(url_, read.parsed->runid);
    track->spec_sha = read.parsed->spec_sha;
    auto origin = state_.analysis_origin(track->uuid, track->record->commit);
    if (!origin) return fail(origin.error());
    track->role = !*origin || (*origin)->from_this_source ? Track::Role::Imported : Track::Role::Foreign;
  }
  return {};
}

Result<void> Mapper::map(const std::vector<Work>& work, int first, ImportBatch& batch) {
  Output out{batch, {}, {}, {}, {}, {}, {}};
  batch_first_ = first;

  // The records first: they say which analysis each file belongs to.
  std::vector<Collect> collects;
  std::vector<Track*> unresolved;
  for (const auto& item : work)
    if (const auto* fold = std::get_if<Collect>(&item)) collects.push_back(*fold);
  for (const auto& item : work) {
    Track* track = nullptr;
    if (const auto* file = std::get_if<Change>(&item)) track = file->track;
    if (const auto* gone = std::get_if<Gone>(&item)) track = gone->track;
    if (!track || !track->flushed || track->role != Track::Role::Unresolved) continue;
    const bool folded =
        std::any_of(collects.begin(), collects.end(), [&](const Collect& fold) { return fold.track == track; });
    if (!folded && std::find(unresolved.begin(), unresolved.end(), track) == unresolved.end())
      unresolved.push_back(track);
  }
  // A spectrometer file that appears for the first time must be told from
  // one that comes too late for an analysis an earlier run folded: the
  // records of those say which they name. A later version of a file the walk
  // already had is too late for nobody (change() says so only where the file
  // first appears), and reads no record.
  const bool settings_arrive = std::any_of(work.begin(), work.end(), [&](const Work& item) {
    const auto* file = std::get_if<Change>(&item);
    return file && file->info.kind == FileKind::Spectrometer &&
           walk_.spectrometer_first(file->info.key) == file->ref.index;
  });
  if (settings_arrive)
    for (Track* track : walk_.flushed()) {
      const bool folded = std::any_of(collects.begin(), collects.end(),
                                      [&](const Collect& fold) { return fold.track == track; });
      if (track->role == Track::Role::Unresolved && track->record && !folded &&
          std::find(unresolved.begin(), unresolved.end(), track) == unresolved.end())
        unresolved.push_back(track);
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
  batch_blobs_ = blobs;
  if (auto r = fetch(std::move(blobs)); !r) return r;

  for (const auto& item : work) {
    if (const auto* fold = std::get_if<Collect>(&item)) {
      if (auto r = collect(*fold, reading, out); !r) return r;
    } else if (const auto* file = std::get_if<Change>(&item)) {
      // The parsers return what they cannot read as an error. What the
      // mapping itself does with a file's content (comparing versions,
      // building the detail) can still meet something the JSON library
      // throws on, or cannot hold: that is the file's fault too.
      try {
        if (auto r = change(*file, out); !r) return r;
      } catch (const Json::exception& e) {
        judge(file->ref, false);
        out.conflict(file->ref, ConflictKind::Unparseable, std::nullopt, std::nullopt,
                     reason(unexpected_content(e).what));
      } catch (const std::bad_alloc& e) {
        judge(file->ref, false);
        out.conflict(file->ref, ConflictKind::Unparseable, std::nullopt, std::nullopt,
                     reason(unexpected_content(e).what));
      }
    } else {
      // A deletion of a file of no analysis, or of an analysis this source
      // imports. (The files of one it does not import have conflicts that say
      // so, which a deletion does not answer.)
      const Gone& gone = std::get<Gone>(item);
      if (!gone.track || gone.track->role == Track::Role::Imported) out.settled.push_back(gone.ref);
    }
  }
  for (const auto& ref : out.settled)
    if (auto r = supersede_before(ref, batch); !r) return r;

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

  judge(record_ref, read.parsed.has_value());
  if (!read.parsed) {
    out.conflict(record_ref, ConflictKind::Unparseable, std::nullopt, read.digest, reason(read.error));
    refuse_files(std::nullopt, "the analysis record " + record_ref.path + " cannot be read");
    auto& refused = out.refused[&track];
    refused.insert(refused.end(), files.begin(), files.end());
    track.role = Track::Role::Broken;
    track.record_unreadable = true;
    release(track);
    return {};
  }
  out.settled.push_back(record_ref);  // readable, whatever becomes of the analysis
  ParsedRecord& record = *read.parsed;
  const Uuid uuid = record.had_uuid ? record.ingest.analysis : ingest::derived_analysis_id(url_, record.runid);
  track.uuid = uuid;
  track.spec_sha = record.spec_sha;

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
    ingest::Evidence host{ingest::Evidence::Kind::Analysis, record_ref.commit, record_ref.path, uuid};
    if (same_source) {
      host = {ingest::Evidence::Kind::Entity, {}, {}, uuid};
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

  std::vector<FileRef> readable;
  for (const auto& file : files) {
    auto text = reader_.blob(file.ref.blob_sha);
    if (!text) return fail(text.error());
    bool could_read = true;
    const auto bad = [&](const Error& error) {
      out.conflict(file.ref, ConflictKind::Unparseable, uuid, sha256(*text), reason(error.what));
      unparseable.push_back(file.ref.path);
      could_read = false;
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
               {ingest::Evidence::Kind::Analysis, record_ref.commit, record_ref.path, uuid});
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
    judge(file.ref, could_read);
    if (could_read) readable.push_back(file.ref);
  }

  // No tags file: later legacy versions kept the tag only in the database.
  if (item.keys.tags.path.empty() && config_.tag_lookup) {
    if (auto name = config_.tag_lookup(uuid); name && !name->empty()) {
      roots.tag.name = std::move(*name);
      detail["tag_from_db"] = true;
    }
  }

  if (record.spec_sha) {
    auto settings = snapshot(*record.spec_sha, track.folded_at);
    if (!settings) return fail(settings.error());
    if (*settings) {
      item.ingest.spectrometer_snapshot = std::move(**settings);
      // The settings file becomes the snapshot of the analyses that name it.
      if (const FileRef* file = walk_.spectrometer(*record.spec_sha))
        silent(*file, ingest::UnitDisposition::Folded,
               {ingest::Evidence::Kind::Analysis, record_ref.commit, record_ref.path, uuid});
    } else {
      detail["spectrometer_file_unavailable"] = *record.spec_sha;  // not in the repository, or unreadable
    }
  }

  // A collection whose record did not arrive in a <COLLECTION> commit, or that
  // never became complete, is one this importer put together.
  item.synthetic_collection = !collection_commit || !track.complete() || track.recovered;
  if (track.recovered) detail["collection_from_first_readable_record"] = true;
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
  out.settled.insert(out.settled.end(), readable.begin(), readable.end());
  track.role = Track::Role::Imported;
  release(track);
  return {};
}

Result<std::optional<ps::SpectrometerSnapshot>> Mapper::snapshot(const std::string& sha1, int folded_at) {
  // A file that first appears after the commit that folded the analysis is
  // not its snapshot, though the walk of this batch may already have seen it:
  // what is attached must not depend on where the batch ends. The file is
  // reported when it arrives (change()).
  const int first = walk_.spectrometer_first(sha1);
  if (first < 0 || first > folded_at) return std::optional<ps::SpectrometerSnapshot>{};
  if (const auto cached = snapshots_.find(sha1); cached != snapshots_.end()) return cached->second;
  const FileRef* file = walk_.spectrometer(sha1);
  if (!file) return std::optional<ps::SpectrometerSnapshot>{};
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
  Json detail{{ingest::kMarkerSynthesized, true}, {"table", "identifier"}, {"identifier", analysis.identifier}};
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
      if (names.sample && names.project) {
        position.sample = names.sample;
        position.project = names.project;
        // A record without a material: the sample goes under the placeholder,
        // as the catalog dump's does (spec section 10.41).
        position.material = names.material.value_or(ingest::kPlaceholderMaterial);
        if (!names.material) detail["placeholder_material"] = ingest::kPlaceholderMaterial;
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
// over the head. It is asked only of a file that can be read: one that
// cannot is a conflict where it comes back, since the conflict of the version
// it repeats was superseded when the file was removed (spec 10.37).
bool Mapper::unchanged(const Change& item) { return item.restored; }

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
                                     std::move(detail_json), std::move(identifier), {}, std::int64_t{ref.index}});
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
      judge(ref, parsed.has_value());
      if (!parsed) {
        bad(parsed.error().what);
        return {};
      }
      slot = std::move(*parsed);
      out.settled.push_back(ref);
      // The file is here for the first time, after an analysis that names it
      // was folded without it: the analysis is stored and cannot take the
      // snapshot now (spec 10.29). Said once, where the file arrives.
      if (walk_.spectrometer_first(item.info.key) != ref.index) return {};
      Json late = Json::array();
      for (const Track* track : walk_.flushed())
        if (track->role == Track::Role::Imported && track->spec_sha == item.info.key && track->folded_at < ref.index)
          late.push_back(track->uuid.str());
      if (!late.empty()) {
        Json detail = reason(ingest::kReasonSpectrometerFileAfterCollection);
        detail["analyses"] = std::move(late);
        out.conflict(ref, ConflictKind::Unparseable, std::nullopt, sha256(*text), detail);
      }
      return {};
    }

    case FileKind::FrozenProduction: {
      const auto [irradiation, level] = split_frozen_production_key(item.info.key);
      const std::string name = "frozen/" + config_.repository_name + "/" + irradiation + "/" + level;
      const ingest::SubjectRef subject =
          ingest::RefObjectKey{std::string(ps::to_string(ps::RefType::Production)), name};
      auto parsed = parse_frozen_production(*text, irradiation, level);
      judge(ref, parsed.has_value());
      if (!parsed) {
        bad(parsed.error().what);
        return {};
      }
      if (unchanged(item)) return {};
      out.settled.push_back(ref);
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
      auto parsed = parse_interpreted_age(*text, item.info.key);
      judge(ref, parsed.has_value());
      if (!parsed) {
        bad(parsed.error().what);
        return {};
      }
      if (unchanged(item)) return {};
      out.settled.push_back(ref);
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
    if (track.record_unreadable && kind == FileKind::Record) {
      // The record again. Readable at last, the analysis starts here.
      if (auto record = parse_record(text, context_)) return recover(item, std::move(*record), sha256(text), out);
      judge(ref, false);
    } else if (track.record_unreadable) {
      out.refused[&track].push_back({kind, ref});
    }
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

  if (kind == FileKind::Data) {
    auto data = parse_data(text);
    judge(ref, data.has_value());
    if (!data) {
      out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(data.error().what));
      return {};
    }
    if (unchanged(item)) return {};
    out.settled.push_back(ref);
    for (auto& blob : data->blobs) out.batch.blobs.push_back({key_of(ref), std::move(blob)});
    Json detail = Json::object();
    if (data->extra_json) detail["extra"] = embedded(*data->extra_json);
    return add_revision(ref, uuid, store_kind, std::move(data->refs), dump(detail), out);
  }

  auto revision = parse_revision(kind, text);
  judge(ref, revision.has_value());
  if (!revision) {
    out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(revision.error().what));
    return {};
  }
  if (unchanged(item)) return {};
  out.settled.push_back(ref);
  Json detail = Json::object();
  if (revision->extra_json) detail["extra"] = embedded(*revision->extra_json);
  return add_revision(ref, uuid, store_kind, std::move(revision->payload), dump(detail), out);
}

// The analysis of `item.track` was folded with a record that cannot be read,
// and its files were refused one by one since. `item` is the first readable
// version of that record: the collection starts at its commit, as a synthetic
// one. It is put together from what the walk has handed over for the analysis
// so far: the first version of each file is its root (or is folded in, for a
// satellite file), every later version is a change, in walk order, as if it
// had been held for a pending collection. The conflicts of the files that can
// now be imported are listed as superseded; the unreadable records before
// this one are superseded like any unreadable version (supersede_before).
//
// What was handed over is every version of the analysis's paths before this
// batch (the walk has them, whatever run mapped them), and what this batch
// refused before `item`. A later run finds the analysis by the same rule:
// resolve() takes the first readable version of the record.
Result<void> Mapper::recover(const Change& item, ParsedRecord record, const Sha256Digest& digest, Output& out) {
  Track& track = *item.track;
  std::vector<SeenFile> had = std::move(out.refused[&track]);
  out.refused.erase(&track);
  for (const auto& path : walk_.paths_of(track.key)) {
    const FileKind kind = classify_path(path).kind;
    if (kind == FileKind::Record) continue;
    for (const auto& version : *walk_.versions(path))
      if (version.index < batch_first_ && !version.blob_sha.empty())
        had.push_back({kind, FileRef{version.index, order_[static_cast<std::size_t>(version.index)], path,
                                     version.blob_sha}});
  }
  const auto before = [](const SeenFile& a, const SeenFile& b) {
    return a.ref.index != b.ref.index ? a.ref.index < b.ref.index : a.ref.path < b.ref.path;
  };
  std::sort(had.begin(), had.end(), before);
  had.erase(std::unique(had.begin(), had.end(),
                        [](const SeenFile& a, const SeenFile& b) {
                          return a.ref.index == b.ref.index && a.ref.path == b.ref.path;
                        }),
            had.end());

  // The collection, and what came after it.
  release(track);
  track.record = item.ref;
  track.folded_at = item.ref.index;
  track.role = Track::Role::Unresolved;
  track.record_unreadable = false;
  track.recovered = true;
  std::vector<Change> later;
  std::map<FileKind, FileRef> last;  // of each kind: the version before, for a rewrite
  std::vector<std::string> blobs;
  for (const auto& file : had) {
    blobs.push_back(file.ref.blob_sha);
    bool first = false;
    if (auto* slot = track.root_slot(file.kind)) {
      first = !slot->has_value();
      if (first) *slot = file.ref;
    } else if (is_rewritable(file.kind)) {
      first = std::none_of(track.satellites.begin(), track.satellites.end(),
                           [&](const SeenFile& seen) { return seen.kind == file.kind; });
      if (first) track.satellites.push_back({file.kind, file.ref});
    }
    std::optional<FileRef> previous;
    if (const auto found = last.find(file.kind); found != last.end() && is_rewritable(file.kind))
      previous = found->second;
    last.insert_or_assign(file.kind, file.ref);
    if (first) continue;
    // Back with what it had when it was removed, as the walk tells it.
    bool restored = false;
    const auto& versions = *walk_.versions(file.ref.path);
    for (std::size_t v = 2; v < versions.size(); ++v)
      if (versions[v].index == file.ref.index)
        restored = versions[v - 1].blob_sha.empty() && versions[v - 2].blob_sha == file.ref.blob_sha;
    later.push_back(Change{classify_path(file.ref.path), file.ref, &track, std::move(previous), true, restored});
  }
  if (record.spec_sha && !snapshots_.contains(*record.spec_sha))
    if (const FileRef* settings = walk_.spectrometer(*record.spec_sha)) blobs.push_back(settings->blob_sha);
  if (auto r = fetch(std::move(blobs)); !r) return r;

  Reading reading;
  Reading::Record read;
  read.parsed = std::move(record);
  read.digest = digest;
  reading.records.insert_or_assign(&track, std::move(read));
  if (auto r = collect(Collect{&track, std::nullopt}, reading, out); !r) return r;
  for (const auto& file : later)
    if (auto r = change(file, out); !r) return r;

  // What could now be read has rows of its own: the conflict that refused it
  // for want of its analysis no longer applies. A file that cannot be read
  // keeps the conflict it has.
  if (track.role == Track::Role::Imported)
    for (const auto& file : had) {
      const auto& versions = *walk_.versions(file.ref.path);
      for (const auto& version : versions)
        if (version.index == file.ref.index && version.read == Version::Read::Yes)
          out.batch.superseded.push_back(key_of(file.ref));
    }
  // The blobs of the batch may have made room for these: what follows reads them again.
  return fetch(batch_blobs_);
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
  auto after = parse_legacy(text);
  std::optional<ParsedRecord> record;
  std::string unreadable;
  if (!after) {
    unreadable = after.error().what;
  } else if (kind == FileKind::Record) {
    auto parsed = parse_record(text, context_);
    if (parsed)
      record = std::move(*parsed);
    else
      unreadable = parsed.error().what;
  }
  judge(ref, unreadable.empty());
  if (!unreadable.empty()) {
    out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(unreadable));
    return {};
  }
  // Removed and added again with what it had: nothing was rewritten.
  if (item.previous && item.previous->blob_sha == ref.blob_sha) return {};
  out.settled.push_back(ref);

  std::optional<Json> before;
  std::optional<ParsedRecord> record_before;
  if (item.previous) {
    auto old_text = reader_.blob(item.previous->blob_sha);
    if (!old_text) return fail(old_text.error());
    if (auto parsed = parse_legacy(*old_text)) before = std::move(*parsed);
    if (kind == FileKind::Record)
      if (auto parsed = parse_record(*old_text, context_)) record_before = std::move(*parsed);
  }

  if (record) {
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
