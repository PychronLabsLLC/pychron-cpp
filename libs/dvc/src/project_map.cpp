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
    std::string error;
    Sha256Digest digest{};
  };
  std::map<const Track*, Record> records;
};

struct Mapper::Output {
  ImportBatch& batch;
  std::map<int, ingest::ChangesetItem> changesets;  // by commit index
  std::set<std::string> revised;                    // paths with a revision earlier in this batch

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
Result<void> Mapper::read_records(const std::vector<Track*>& tracks, Reading& reading) {
  for (std::size_t begin = 0; begin < tracks.size(); begin += kRecordsPerFetch) {
    const std::size_t end = std::min(tracks.size(), begin + kRecordsPerFetch);
    std::vector<std::string> blobs;
    for (std::size_t i = begin; i < end; ++i) blobs.push_back(tracks[i]->record->blob_sha);
    if (auto r = fetch(std::move(blobs)); !r) return r;
    for (std::size_t i = begin; i < end; ++i) {
      auto text = reader_.blob(tracks[i]->record->blob_sha);
      if (!text) return fail(text.error());
      Reading::Record read;
      read.digest = sha256(*text);
      auto parsed = parse_record(*text, context_);
      if (parsed)
        read.parsed = std::move(*parsed);
      else
        read.error = parsed.error().what;
      reading.records.insert_or_assign(tracks[i], std::move(read));
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
  if (auto r = read_records(tracks, reading); !r) return r;
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
    if (track->role == Track::Role::Imported) {
      owners_.try_emplace(track->uuid, track);
      runids_.try_emplace(read.parsed->runid, track->uuid);
    }
  }
  return {};
}

Result<void> Mapper::map(const std::vector<Work>& work, ImportBatch& batch) {
  Output out{batch, {}, {}};

  // The records first: they say which analysis each file belongs to.
  std::vector<Track*> collects, unresolved;
  for (const auto& item : work)
    if (const auto* fold = std::get_if<Collect>(&item)) collects.push_back(fold->track);
  for (const auto& item : work) {
    const auto* file = std::get_if<Change>(&item);
    if (!file || !file->track || !file->track->flushed || file->track->role != Track::Role::Unresolved) continue;
    const auto listed = [&](const std::vector<Track*>& list) {
      return std::find(list.begin(), list.end(), file->track) != list.end();
    };
    if (!listed(collects) && !listed(unresolved)) unresolved.push_back(file->track);
  }
  Reading reading;
  if (auto r = read_records(collects, reading); !r) return r;
  if (auto r = resolve(unresolved); !r) return r;

  // Then every other file of the batch, in one read.
  std::vector<std::string> blobs;
  for (const Track* track : collects) {
    for (const auto& file : collection_files(*track)) blobs.push_back(file.ref.blob_sha);
    const auto& read = reading.records.at(track);
    if (read.parsed && read.parsed->spec_sha && !snapshots_.contains(*read.parsed->spec_sha))
      if (const FileRef* settings = walk_.spectrometer(*read.parsed->spec_sha)) blobs.push_back(settings->blob_sha);
  }
  for (const auto& item : work)
    if (const auto* file = std::get_if<Change>(&item); file && !(file->track && !file->track->flushed))
      blobs.push_back(file->ref.blob_sha);
  if (auto r = fetch(std::move(blobs)); !r) return r;

  for (const auto& item : work) {
    if (const auto* fold = std::get_if<Collect>(&item)) {
      if (auto r = collect(*fold->track, reading, out); !r) return r;
    } else if (auto r = change(std::get<Change>(item), out); !r) {
      return r;
    }
  }

  for (auto& [index, changeset] : out.changesets) {
    // A commit that only touches reference data is a reference changeset.
    const bool reference =
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

Result<void> Mapper::collect(Track& track, Reading& reading, Output& out) {
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

  // A second copy of an analysis this walk already imported (a file set moved
  // or copied to another run id).
  if (const auto owner = owners_.find(uuid); owner != owners_.end() && owner->second != &track) {
    Json detail = reason("this analysis is already imported from " + owner->second->record->path);
    detail["uuid"] = uuid.str();
    out.conflict(record_ref, ConflictKind::IdentityClash, uuid, read.digest, detail);
    track.role = Track::Role::Foreign;
    release(track);
    return {};
  }
  // Two analyses cannot share a run id.
  if (const auto taken = runids_.find(record.runid); taken != runids_.end() && taken->second != uuid) {
    Json detail = reason("run id " + record.runid + " is already imported with another uuid");
    detail["uuid"] = uuid.str();
    detail["imported_uuid"] = taken->second.str();
    out.conflict(record_ref, ConflictKind::IdentityClash, taken->second, read.digest, detail);
    refuse_files(uuid, "the analysis " + record.runid + " was not imported: its run id is taken");
    track.role = Track::Role::Broken;
    release(track);
    return {};
  }

  auto meta = commit(record_ref.commit);
  if (!meta) return fail(meta.error());
  const ingest::GitWho who = (*meta)->author;
  const bool collection_commit = std::string_view((*meta)->message).starts_with(kCollectionTag);

  // In the store from another source: it joins this repository, nothing else.
  auto origin = state_.analysis_origin(uuid, record_ref.commit);
  if (!origin) return fail(origin.error());
  if (*origin && !(*origin)->from_this_source) {
    out.batch.memberships.push_back({uuid, key_of(record_ref), who, {config_.repository_name}});
    const std::string& stored = (*origin)->record_blob_sha;
    if (!stored.empty() && stored != record_ref.blob_sha) {
      Json detail = reason("the record differs from the one this analysis was imported from");
      detail["imported_blob"] = stored;
      detail["blob"] = record_ref.blob_sha;
      out.conflict(record_ref, ConflictKind::IdentityClash, uuid, read.digest, detail);
    }
    track.role = Track::Role::Foreign;
    release(track);
    return {};
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
  if (!record.notes.empty()) detail["notes"] = record.notes;
  std::vector<std::pair<int, std::string>> commits{{record_ref.index, record_ref.commit}};
  Json unparseable = Json::array();

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
        break;
      }
      default: {
        auto revision = parse_revision(file.kind, *text);
        if (!revision) {
          bad(revision.error());
          break;
        }
        extra(revision->extra_json);
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
    if (*settings)
      item.ingest.spectrometer_snapshot = std::move(**settings);
    else
      detail["spectrometer_file_unavailable"] = *record.spec_sha;  // not in the repository, or unreadable
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

  if (config_.catalog_from_repos) synthesize_catalog(record, item.ingest, record_ref, out);
  out.batch.analyses.push_back(std::move(item));

  owners_.insert_or_assign(uuid, &track);
  runids_.insert_or_assign(record.runid, uuid);
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
void Mapper::synthesize_catalog(const ParsedRecord& record, const ps::AnalysisIngest& analysis, const FileRef& from,
                                Output& out) {
  const auto once = [&](std::string what) { return sent_.insert(std::move(what)).second; };
  auto& catalog = out.batch.catalog;
  if (once("mass_spectrometer\n" + analysis.mass_spectrometer)) {
    ps::MassSpectrometerSpec spec;
    spec.name = analysis.mass_spectrometer;
    catalog.push_back(ingest::MassSpecItem{std::move(spec)});
  }
  if (analysis.extract_device && once("extract_device\n" + *analysis.extract_device))
    catalog.push_back(ingest::ExtractDeviceItem{*analysis.extract_device});
  if (!once("identifier\n" + analysis.identifier)) return;

  const auto& names = record.catalog;
  Json detail{{"synthesized", true}, {"table", "identifier"}, {"identifier", analysis.identifier}};
  detail["from"] = Json{{"commit", from.commit}, {"path", from.path}};
  bool placed = false;
  const bool irradiated = names.irradiation && names.irradiation_level && names.irradiation_position;
  if (analysis.analysis_type == "unknown" && irradiated) {
    const std::string where =
        *names.irradiation + "\n" + *names.irradiation_level + "\n" + std::to_string(*names.irradiation_position);
    // One identifier per irradiation position: a second one there gets none.
    if (positions_.try_emplace(where, analysis.identifier).first->second == analysis.identifier) {
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
      detail["position_taken_by"] = positions_.at(where);
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
}

// ---------------------------------------------------------------- later changes

Result<bool> Mapper::is_head(const FileRef& ref, const ingest::SubjectRef& subject, ps::Kind kind, Output& out) {
  // The stored head is what it was before this batch: only the first change
  // of a file in the batch can be compared with it.
  if (!out.revised.insert(ref.path).second) return false;
  auto head = state_.head_blob_sha(subject, kind);
  if (!head) return fail(head.error());
  return *head && **head == ref.blob_sha;
}

Result<void> Mapper::add_revision(const FileRef& ref, ingest::SubjectRef subject, ps::Kind kind,
                                  ps::RevisionPayload payload, std::string detail_json, Output& out) {
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
  found->second.revisions.push_back(
      {key_of(ref), std::move(subject), kind, std::move(payload), std::move(detail_json)});
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
      auto same = is_head(ref, subject, ps::Kind::RefValue, out);
      if (!same) return fail(same.error());
      if (*same) return {};
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
      auto same = is_head(ref, subject, ps::Kind::InterpretedAge, out);
      if (!same) return fail(same.error());
      if (*same) return {};
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

  if (kind == FileKind::Record) {
    auto parsed = parse_record(text, context_);
    if (!parsed) {
      out.conflict(ref, ConflictKind::Unparseable, uuid, sha256(text), reason(parsed.error().what));
      return {};
    }
    const Uuid now = parsed->had_uuid ? parsed->ingest.analysis : ingest::derived_analysis_id(url_, parsed->runid);
    if (now != uuid) {
      Json detail = reason("the record now carries another uuid; the imported analysis is unchanged");
      detail["imported_uuid"] = uuid.str();
      detail["uuid"] = now.str();
      out.conflict(ref, ConflictKind::IdentityClash, uuid, sha256(text), detail);
    } else {
      out.conflict(ref, ConflictKind::HandEdit, uuid, sha256(text),
                   reason("the record was rewritten after collection; the imported analysis is unchanged"));
    }
    return {};
  }
  if (kind == FileKind::Extraction || kind == FileKind::PeakCenter || kind == FileKind::Monitor) {
    out.conflict(ref, ConflictKind::HandEdit, uuid, sha256(text),
                 reason(std::string("the ") + kind_name(kind) +
                        " file changed after collection; the imported analysis is unchanged"));
    return {};
  }

  const ps::Kind store_kind = kind == FileKind::Data ? ps::Kind::Signals : *revision_kind(kind);
  auto same = is_head(ref, ingest::SubjectRef{uuid}, store_kind, out);
  if (!same) return fail(same.error());
  if (*same) return {};

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

}  // namespace pychron::dvc::detail
