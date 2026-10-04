// The content-free half of the project repository adapter: which file of
// which commit belongs to a collection, which is a later change, which is
// nothing (project_import.hpp).

#include <algorithm>

#include "project_import.hpp"

namespace pychron::dvc::detail {

namespace {

bool is_satellite(FileKind kind) {
  return kind == FileKind::Extraction || kind == FileKind::PeakCenter || kind == FileKind::Monitor;
}

}  // namespace

std::optional<FileRef>* Track::root_slot(FileKind kind) {
  switch (kind) {
    case FileKind::Record: return &record;
    case FileKind::Data: return &data;
    case FileKind::Intercepts: return &intercepts;
    case FileKind::Baselines: return &baselines;
    case FileKind::Blanks: return &blanks;
    case FileKind::IcFactors: return &icfactors;
    case FileKind::Tags: return &tags;
    default: return nullptr;
  }
}

std::optional<FileRef> Track::replace_latest(FileKind kind, const FileRef& ref) {
  for (auto& file : latest) {
    if (file.kind != kind) continue;
    std::optional<FileRef> before = std::move(file.ref);
    file.ref = ref;
    return before;
  }
  latest.push_back({kind, ref});
  return std::nullopt;
}

bool Walk::apply(int index, std::span<const GitChange> changes, std::vector<Work>* out) {
  std::vector<Track*> touched;
  bool record_rewritten = false;
  applied_ = index;
  const auto note = [&](const GitChange& change, Ledger::Seen as) {
    if (ledger_) ledger_->changes.push_back({change.commit, change.path, change.blob_sha, as});
  };
  for (const auto& entry : changes) {
    if (entry.status == 'D') {
      // A deleted file adds nothing. What it held is remembered: added again
      // with the same content, it has not changed.
      note(entry, Ledger::Seen::Deleted);
      const auto had = versions_.find(entry.path);
      if (had == versions_.end() || had->second.back().blob_sha.empty()) continue;  // never seen, or gone already
      had->second.push_back({index, {}, Version::Read::Unknown});
      // The deletion of a recognised file is handed on (Gone).
      PathInfo gone = classify_path(entry.path);
      FileRef at{index, entry.commit, entry.path, {}};
      switch (gone.kind) {
        case FileKind::Ignored:
        case FileKind::Unknown:
          continue;
        case FileKind::Spectrometer:
        case FileKind::InterpretedAge:
        case FileKind::FrozenProduction:
          if (out) out->push_back(Gone{std::move(gone), std::move(at), nullptr});
          continue;
        default:
          break;
      }
      const auto owner = tracks_.find(gone.key);
      if (owner == tracks_.end()) continue;
      if (owner->second.flushed) {
        if (out) out->push_back(Gone{std::move(gone), std::move(at), &owner->second});
      } else {
        owner->second.later.push_back({gone.kind, std::move(at)});  // with the collection, in walk order
      }
      continue;
    }
    bool restored = false;
    auto& had = versions_[entry.path];
    Version::Read known = Version::Read::Unknown;
    if (!had.empty()) {
      if (had.back().blob_sha == entry.blob_sha) {
        if (ledger_)
          note(entry, classify_path(entry.path).kind == FileKind::Ignored ? Ledger::Seen::Ignored
                                                                         : Ledger::Seen::Repeated);
        continue;
      }
      // Gone, and back with the blob it had when it was removed. (A deletion
      // always follows a version that was there.)
      if (had.back().blob_sha.empty() && had[had.size() - 2].blob_sha == entry.blob_sha) {
        restored = true;
        known = had[had.size() - 2].read;
      }
    }
    had.push_back({index, entry.blob_sha, known});

    PathInfo info = classify_path(entry.path);
    FileRef ref{index, entry.commit, entry.path, entry.blob_sha};
    // A restored file repeats what its path held: for the ledger it is the
    // unit it repeats, whatever is made of it below.
    note(entry, info.kind == FileKind::Ignored ? Ledger::Seen::Ignored
                : restored                     ? Ledger::Seen::Repeated
                                               : Ledger::Seen::Taken);
    switch (info.kind) {
      case FileKind::Ignored:
        continue;
      case FileKind::Spectrometer:
        spectrometers_[info.key] = ref;
        spectrometer_first_.try_emplace(info.key, index);
        [[fallthrough]];
      case FileKind::Unknown:
      case FileKind::InterpretedAge:
      case FileKind::FrozenProduction:
        if (out) out->push_back(Change{std::move(info), std::move(ref), nullptr, std::nullopt, false, restored});
        continue;
      default:
        break;
    }

    Track& track = tracks_[info.key];
    if (track.key.empty()) {
      track.key = info.key;
      track.key_is_uuid = info.key_is_uuid;
    }
    const FileKind kind = info.kind;
    std::optional<FileRef> previous;
    if (kind == FileKind::Record || is_satellite(kind)) previous = track.replace_latest(kind, ref);
    if (track.flushed) {
      if (kind == FileKind::Record) record_rewritten = true;
      if (out)
        out->push_back(Change{std::move(info), std::move(ref), &track, std::move(previous), false, restored});
      continue;
    }
    if (auto* slot = track.root_slot(kind)) {
      if (!*slot) {
        if (kind == FileKind::Record) pending_.emplace(index, track.key);
        *slot = std::move(ref);
      } else {
        track.later.push_back({kind, std::move(ref), std::move(previous), restored});
      }
    } else {
      const bool first = is_satellite(kind) &&
                         std::none_of(track.satellites.begin(), track.satellites.end(),
                                      [kind](const SeenFile& file) { return file.kind == kind; });
      if (first)
        track.satellites.push_back({kind, std::move(ref)});
      else
        track.later.push_back({kind, std::move(ref), std::move(previous), restored});
    }
    if (std::find(touched.begin(), touched.end(), &track) == touched.end()) touched.push_back(&track);
  }
  for (Track* track : touched)
    if (track->complete()) flush(*track, out);
  // The bounded wait: counted in commits of the walk, so the same analyses
  // are folded at the same commits however the walk is cut into batches.
  while (wait_ > 0 && !pending_.empty() && pending_.begin()->first + wait_ <= index)
    flush(tracks_.at(pending_.begin()->second), out);
  return record_rewritten;
}

void Walk::flush(Track& track, std::vector<Work>* out) {
  track.flushed = true;
  track.folded_at = applied_;
  if (track.record) pending_.erase({track.record->index, track.key});
  flushed_.push_back(&track);
  if (out) {
    Collect fold{&track, std::nullopt};
    for (const auto& file : track.latest)
      if (file.kind == FileKind::Record && track.record && file.ref.blob_sha != track.record->blob_sha)
        fold.record_now = file.ref;
    out->push_back(std::move(fold));
    for (auto& file : track.later) {
      PathInfo info = classify_path(file.ref.path);
      if (file.ref.blob_sha.empty())
        out->push_back(Gone{std::move(info), std::move(file.ref), &track});
      else
        out->push_back(
            Change{std::move(info), std::move(file.ref), &track, std::move(file.previous), true, file.restored});
    }
  }
  track.later.clear();
  track.later.shrink_to_fit();
}

void Walk::assume_written() {
  for (const auto& entry : pending()) flush(*entry, nullptr);
}

std::vector<Track*> Walk::pending() {
  std::vector<Track*> tracks;
  tracks.reserve(pending_.size());
  for (const auto& entry : pending_) tracks.push_back(&tracks_.at(entry.second));
  return tracks;
}

void Walk::force(Track& track, std::vector<Work>& out) {
  if (!track.flushed) flush(track, &out);
}

void Walk::orphans(std::vector<Work>& out) {
  for (auto& [key, track] : tracks_) {
    if (track.flushed || track.record) continue;
    Track* owner = &track;
    const auto add = [&](FileKind kind, const FileRef& ref) {
      PathInfo info;
      info.kind = kind;
      info.key = track.key;
      info.key_is_uuid = track.key_is_uuid;
      out.push_back(Change{std::move(info), ref, owner, std::nullopt, false, false});
    };
    if (track.data) add(FileKind::Data, *track.data);
    if (track.intercepts) add(FileKind::Intercepts, *track.intercepts);
    if (track.baselines) add(FileKind::Baselines, *track.baselines);
    if (track.blanks) add(FileKind::Blanks, *track.blanks);
    if (track.icfactors) add(FileKind::IcFactors, *track.icfactors);
    if (track.tags) add(FileKind::Tags, *track.tags);
    for (const auto& file : track.satellites) add(file.kind, file.ref);
    for (const auto& file : track.later)
      if (!file.ref.blob_sha.empty()) add(file.kind, file.ref);  // a deletion is not a file
  }
}

std::vector<Version>* Walk::versions(const std::string& path) {
  const auto it = versions_.find(path);
  return it == versions_.end() ? nullptr : &it->second;
}

std::vector<std::string> Walk::paths_of(const std::string& key) const {
  std::vector<std::string> paths;
  for (const auto& [path, versions] : versions_)
    if (classify_path(path).key == key) paths.push_back(path);
  std::sort(paths.begin(), paths.end());
  return paths;
}

int Walk::spectrometer_first(const std::string& sha1) const {
  const auto it = spectrometer_first_.find(sha1);
  return it == spectrometer_first_.end() ? -1 : it->second;
}

const FileRef* Walk::spectrometer(const std::string& sha1) const {
  const auto it = spectrometers_.find(sha1);
  return it == spectrometers_.end() ? nullptr : &it->second;
}

}  // namespace pychron::dvc::detail
