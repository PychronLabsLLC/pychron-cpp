#pragma once

// Installing a profile into a root directory (installation wizard spec
// sections 3.2, 3.4), in two phases: plan_install() renders and checks
// everything (every .toml it would write must parse) without touching the
// disk; apply_install() writes. An existing file is never overwritten by an
// install: it is kept, and the plan says so. A reconfigure rewrites only
// files still exactly as the last install wrote them (by hash) and puts the
// new version beside an edited one as <name>.new.
//
// The root keeps <root>/.pychron/install.toml: the profiles and versions,
// the answers (secrets left out) and the hash of each file written.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/setup/profile.hpp"

namespace pychron::setup {

struct PlannedFile {
  enum class Action {
    Write,     // new
    Same,      // exists with this content already
    Keep,      // exists with other content: left alone (install never overwrites)
    Update,    // reconfigure: unchanged since the last install, rewritten
    Conflict,  // reconfigure: edited since the last install; written as <to>.new
  };
  std::filesystem::path to;  // relative to the root
  std::string content;
  bool secret = false;
  std::string profile;
  Action action = Action::Write;
};
std::string_view to_string(PlannedFile::Action a) noexcept;

struct InstallPlan {
  std::filesystem::path root;
  ResolvedProfile profile;
  Answers answers;
  std::vector<PlannedFile> files;
  // Files still holding "SIMULATION PLACEHOLDER" or "CONFIRM" markers.
  std::vector<std::filesystem::path> placeholders;
};

struct PlanOptions {
  bool reconfigure = false;
  // Leave files marked secret out of the plan (a reconfigure that was not
  // given the secret again keeps the file it already has).
  bool skip_secret_files = false;
};

Result<InstallPlan> plan_install(const ProfileLibrary& library, const ResolvedProfile& profile, const Answers& answers,
                                 const std::filesystem::path& root, PlanOptions options = {});

struct InstallReport {
  std::vector<std::filesystem::path> written, kept, same, updated, conflicts;  // conflicts: the .new files
};

Result<InstallReport> apply_install(const InstallPlan& plan);

struct InstallRecord {
  std::string profile;
  std::map<std::string, std::int64_t> versions;
  Answers answers;                                 // no secrets
  std::map<std::string, std::string> files;        // relative path -> sha256 hex
};
std::filesystem::path record_path(const std::filesystem::path& root);
// NotConnected-free: Io when the record is missing, Config when it is malformed.
Result<InstallRecord> read_install_record(const std::filesystem::path& root);

std::string sha256_hex(std::string_view bytes);

}  // namespace pychron::setup
