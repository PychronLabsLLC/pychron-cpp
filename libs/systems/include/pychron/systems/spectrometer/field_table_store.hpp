#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"

namespace pychron::spectrometer {

// Versioned persistence for one named table:
//
//   <tables_root>/<name>/<version>.toml   immutable snapshots
//   <tables_root>/<name>/current          text file holding the active version
//
// Versions are UTC timestamps "YYYYMMDDTHHMMSSZ" (suffixed "-N" on
// collision), so lexical order is chronological. Saving never overwrites an
// existing version; restore() only moves the `current` pointer.
class FieldTableStore {
 public:
  using WallTime = std::chrono::system_clock::time_point;

  FieldTableStore(std::filesystem::path tables_root, std::string name);

  const std::string& name() const noexcept { return name_; }
  std::filesystem::path directory() const { return root_ / name_; }

  // Writes a new version stamped `when` and makes it current.
  Result<std::string> save(const FieldTable& table, WallTime when);

  // All versions, oldest first.
  Result<std::vector<std::string>> versions() const;
  Result<std::string> current_version() const;

  Result<FieldTable> load() const;  // current
  Result<FieldTable> load(const std::string& version) const;

  // Points `current` at an existing, parseable version.
  Result<void> restore(const std::string& version);

 private:
  std::filesystem::path version_path(const std::string& version) const;
  Result<void> set_current(const std::string& version);

  std::filesystem::path root_;
  std::string name_;
};

// "YYYYMMDDTHHMMSSZ" in UTC.
std::string format_version(FieldTableStore::WallTime when);

}  // namespace pychron::spectrometer
