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

void Walk::apply(int index, std::span<const GitChange> changes, std::vector<Work>* out) {
  std::vector<Track*> touched;
  for (const auto& entry : changes) {
    if (entry.status == 'D') {
      // A deleted file adds nothing; added again later, it is a change.
      last_blob_.erase(entry.path);
      continue;
    }
    const auto [seen, first_time] = last_blob_.try_emplace(entry.path, entry.blob_sha);
    if (!first_time) {
      if (seen->second == entry.blob_sha) continue;
      seen->second = entry.blob_sha;
    }

    PathInfo info = classify_path(entry.path);
    FileRef ref{index, entry.commit, entry.path, entry.blob_sha};
    switch (info.kind) {
      case FileKind::Ignored:
        continue;
      case FileKind::Spectrometer:
        spectrometers_[info.key] = ref;
        [[fallthrough]];
      case FileKind::Unknown:
      case FileKind::InterpretedAge:
      case FileKind::FrozenProduction:
        if (out) out->push_back(Change{std::move(info), std::move(ref), nullptr});
        continue;
      default:
        break;
    }

    Track& track = tracks_[info.key];
    if (track.key.empty()) {
      track.key = info.key;
      track.key_is_uuid = info.key_is_uuid;
    }
    if (track.flushed) {
      if (out) out->push_back(Change{std::move(info), std::move(ref), &track});
      continue;
    }
    const FileKind kind = info.kind;
    if (auto* slot = track.root_slot(kind)) {
      if (!*slot) {
        if (kind == FileKind::Record) pending_.emplace(index, &track);
        *slot = std::move(ref);
      } else {
        track.later.push_back({kind, std::move(ref)});
      }
    } else {
      const bool first = is_satellite(kind) &&
                         std::none_of(track.satellites.begin(), track.satellites.end(),
                                      [kind](const SeenFile& file) { return file.kind == kind; });
      (first ? track.satellites : track.later).push_back({kind, std::move(ref)});
    }
    if (std::find(touched.begin(), touched.end(), &track) == touched.end()) touched.push_back(&track);
  }
  for (Track* track : touched)
    if (track->complete()) flush(*track, out);
}

void Walk::flush(Track& track, std::vector<Work>* out) {
  track.flushed = true;
  if (track.record) pending_.erase({track.record->index, &track});
  flushed_.push_back(&track);
  if (out) {
    out->push_back(Collect{&track});
    for (auto& file : track.later) {
      PathInfo info = classify_path(file.ref.path);
      out->push_back(Change{std::move(info), std::move(file.ref), &track});
    }
  }
  track.later.clear();
  track.later.shrink_to_fit();
}

void Walk::assume_written() {
  for (const auto& entry : pending()) flush(*entry, nullptr);
}

std::optional<int> Walk::earliest_pending() const {
  if (pending_.empty()) return std::nullopt;
  return pending_.begin()->first;
}

std::vector<Track*> Walk::pending() const {
  std::vector<Track*> tracks;
  tracks.reserve(pending_.size());
  for (const auto& entry : pending_) tracks.push_back(entry.second);
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
      out.push_back(Change{std::move(info), ref, owner});
    };
    if (track.data) add(FileKind::Data, *track.data);
    if (track.intercepts) add(FileKind::Intercepts, *track.intercepts);
    if (track.baselines) add(FileKind::Baselines, *track.baselines);
    if (track.blanks) add(FileKind::Blanks, *track.blanks);
    if (track.icfactors) add(FileKind::IcFactors, *track.icfactors);
    if (track.tags) add(FileKind::Tags, *track.tags);
    for (const auto& file : track.satellites) add(file.kind, file.ref);
    for (const auto& file : track.later) add(file.kind, file.ref);
  }
}

const FileRef* Walk::spectrometer(const std::string& sha1) const {
  const auto it = spectrometers_.find(sha1);
  return it == spectrometers_.end() ? nullptr : &it->second;
}

}  // namespace pychron::dvc::detail
