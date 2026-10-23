// A scratch copy of a lab as the repository has it. What a session run by hand
// leaves beside the examples (a locked valve, a hole autocenter found, records,
// a machine's own overrides) is git-ignored, so it never shows, but a plain
// recursive copy would carry it into the test. Mirrors `.gitignore`.
#pragma once

#include <filesystem>
#include <string>

namespace pychron::testing {

// True for what a session writes beside a lab or a site keeps for itself:
// <lab>/data, <lab>/notifications.toml, any stage_corrections, snapshots or
// camera_scales folder, any *.state.toml or *.local.toml file. `rel` is
// relative to the lab.
[[nodiscard]] inline bool is_lab_runtime_state(const std::filesystem::path& rel) {
  if (rel == "data" || rel == "notifications.toml") return true;
  for (const auto& part : rel) {
    if (part == "stage_corrections" || part == "snapshots" || part == "camera_scales") return true;
  }
  const std::string name = rel.filename().string();
  return name.ends_with(".state.toml") || name.ends_with(".local.toml");
}

// Copies the lab at `from` to `to` (created; an existing file there is
// overwritten), leaving out its runtime state.
inline void copy_lab(const std::filesystem::path& from, const std::filesystem::path& to) {
  namespace fs = std::filesystem;
  fs::create_directories(to);
  for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator(); ++it) {
    const fs::path rel = fs::relative(it->path(), from);
    if (is_lab_runtime_state(rel)) {
      if (it->is_directory()) it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory()) {
      fs::create_directories(to / rel);
    } else {
      fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing);
    }
  }
}

}  // namespace pychron::testing
