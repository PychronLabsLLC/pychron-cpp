#pragma once

// The install steps elctl init and the setup wizard share: where the shipped
// profiles are, the default name and folder of an install, which questions
// are asked, the database URL a data-reduction install opens, and recording
// an install in the site config.

#include <filesystem>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::setup {

// The shipped profiles and example configs. $PYCHRON_PROFILES_DIR and
// $PYCHRON_EXAMPLES_DIR win; then an installed layout next to the program
// (<prefix>/bin/x -> <prefix>/share/pychron, a macOS bundle's
// Contents/MacOS/x -> Contents/Resources); then the source tree the program
// was built from.
struct Resources {
  std::filesystem::path profiles, examples;
};
Resources find_resources();
Resources find_resources(const std::filesystem::path& executable_dir);

// The folder a program keeps a lab's runtime state in (line state, autocenter
// corrections, camera scales, snapshots, default data): `lab` itself, unless
// `lab` is `examples` or a folder under it; then the same place under
// `user_dir` (pychron::default_examples_state_dir), since the shipped
// examples are never written to. Folders are compared as files
// (fs::equivalent, from `lab` up), so a relative or linked path, or other
// letter case on a disk that ignores it, is still the examples. An empty
// `user_dir` is <temp>/pychron/examples.
[[nodiscard]] std::filesystem::path lab_state_dir(const std::filesystem::path& lab,
                                                  const std::filesystem::path& examples,
                                                  const std::filesystem::path& user_dir);

// Where the field tables of a spectrometer whose config is in `root` are read
// and saved (spectrometer::cfg::SpectrometerData::table_root):
// lab_state_dir(root, ...). When that is not `root` (the shipped examples),
// each table under <root>/tables is first copied there, unless the state
// folder already has that table (its `current`): what a session saved is
// kept, and a version shipped later is not seen until that copy is removed.
// Io error when a table cannot be copied.
[[nodiscard]] Result<std::filesystem::path> field_table_root(const std::filesystem::path& root,
                                                             const std::filesystem::path& examples,
                                                             const std::filesystem::path& user_dir);

// The pychron version these programs were built as ("0.2.0").
std::string_view version() noexcept;

std::filesystem::path home_dir();
// "data-reduction" for data reduction, else the profile's name.
std::string default_install_name(const Profile& profile);
// ~/Documents/Pychron for data reduction, ~/Pychron/<name> for an instrument.
std::filesystem::path default_root(const Profile& profile, const std::string& name);
// What the templates get besides the answers: install_name and root.
Answers builtin_answers(const std::string& name, const std::filesystem::path& root);

// Whether `q` is asked given the answers so far (its `when` holds).
bool is_asked(const Question& q, const Answers& so_far);

// A reconfigure without the secrets again: blanks each unanswered secret so
// the files that hold them are kept (PlanOptions::skip_secret_files). True
// when any was blanked.
bool keep_unanswered_secrets(const ResolvedProfile& profile, Answers& given);

// A data-reduction install's database: sqlite:<file>, where <file> is the
// `db_file` answer (absent: data/pychron.db) taken against `root` unless it is
// absolute, or
// postgresql://user[:password]@host:port/db from the answers (the password
// percent-encoded, only when `with_password`).
std::string database_url_for(const Answers& answers, const std::filesystem::path& root, bool with_password);
std::string percent_encode(std::string_view text);

// Adds or replaces `install` in the site config at `site_path`; it becomes
// the default when there is none yet.
Result<void> register_install(const SiteInstall& install, const std::filesystem::path& site_path);

}  // namespace pychron::setup
