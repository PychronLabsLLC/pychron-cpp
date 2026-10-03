#pragma once

// Setup profiles (installation wizard spec section 3.1): a directory with a
// profile.toml manifest and the files it renders or copies into an install
// root. Profiles compose through `includes`; ProfileLibrary::resolve()
// flattens a profile with everything it includes (depth first, each profile
// once), so its questions and files are the union.
//
//   name = "argus"
//   title = "Thermo Argus VI"
//   kind = "instrument"          # instrument | data_reduction | fragment
//   summary = "..."
//   version = 1
//   includes = ["lab-common"]
//   groups = ["Simulation", "Instrument connection", "Detectors"]
//                                # optional: the order the wizard asks groups in
//
//   [values]                     # fixed answers the templates use, never asked
//   instrument = "Argus VI"
//
//   [[questions]]
//   id = "host"  prompt = "..."  type = "host"  default = "192.168.0.10"
//   group = "Connection"  help = "..."  when = "not simulation"
//   choices = [...]      # type choice
//   labels = [...]       # optional: how the wizard shows each choice
//   columns = [...]      # type table: each row is a table with these keys
//
//   [[files]]
//   template = "spectrometer.toml"   # rendered, under the profile directory
//   copy = "@examples/plans/"        # or copied verbatim; "@examples/" is the
//                                    # shipped example configs, a trailing '/' a directory
//   to = "spectrometer.toml"         # under the install root
//   copy = "{{ line_file }}"         # or a file the user named (a path answer)
//   when = "simulation"              # optional condition over the answers
//   secret = true                    # written owner-only, never summarised
//   check = "line"                   # loaded before anything is written:
//                                    # "line" (extraction-line config) or
//                                    # "canvas" (checked against the line)

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/setup/template.hpp"

namespace pychron::setup {

enum class ProfileKind { Instrument, DataReduction, Fragment };
std::string_view to_string(ProfileKind k) noexcept;

enum class QuestionType { String, Host, Port, Int, Float, Bool, Choice, Path, Secret, List, Table };
std::string_view to_string(QuestionType t) noexcept;

struct Question {
  std::string id, prompt, help, group, when;
  QuestionType type = QuestionType::String;
  std::optional<Value> default_value;
  std::vector<std::string> choices;  // Choice
  std::vector<std::string> labels;   // Choice: what the wizard shows for each (optional)
  std::vector<std::string> columns;  // Table
  std::string profile;               // the profile that asked it
};

struct FileSpec {
  std::string template_path;  // relative to the profile directory; or
  std::string copy;           // relative to the profile directory, or "@examples/..."
  std::string to;             // relative to the install root
  std::string when;
  bool secret = false;
  std::string check;  // "", "line" or "canvas"
  std::string profile;
  std::filesystem::path profile_dir;
};

struct Profile {
  std::string name, title, summary;
  ProfileKind kind = ProfileKind::Fragment;
  std::int64_t version = 1;
  std::vector<std::string> includes;
  std::vector<std::string> groups;  // wizard page order hint
  Answers values;
  std::vector<Question> questions;
  std::vector<FileSpec> files;
  std::filesystem::path dir;
};

Result<Profile> load_profile(const std::filesystem::path& dir);

// A profile with everything it includes, flattened.
struct ResolvedProfile {
  Profile top;
  std::vector<std::string> chain;                   // names, includes first
  std::map<std::string, std::int64_t> versions;     // by name
  Answers values;                                   // merged; the including profile wins
  // Group order: the including profile's hints first, then each include's;
  // groups no hint names follow in the order their first question comes.
  std::vector<std::string> groups;
  std::vector<Question> questions;                  // includes' first
  std::vector<FileSpec> files;
};

class ProfileLibrary {
 public:
  // Every subdirectory of `root` holding a profile.toml. `examples` is what
  // "@examples/" names. Config error listing every profile that failed.
  static Result<ProfileLibrary> load(const std::filesystem::path& root, const std::filesystem::path& examples);

  std::vector<const Profile*> list() const;  // by name
  const Profile* find(const std::string& name) const;
  // Config error on an unknown include, an include cycle, a question id
  // asked twice with different types, or two profiles writing one file.
  Result<ResolvedProfile> resolve(const std::string& name) const;
  const std::filesystem::path& examples() const noexcept { return examples_; }

 private:
  std::map<std::string, Profile> profiles_;
  std::filesystem::path examples_;
};

// --- answers ----------------------------------------------------------------

// Every question asked (its `when` holds given the answers before it) has a
// value: given, else its default. Given values are coerced to the question's
// type (from text, as typed at a prompt or in --set id=value). Config error
// listing every problem: an unknown id, a value of the wrong type, a choice
// not offered, a port out of range, a required question without a value.
// `builtins` (install_name, ...) and the profile's values come first and
// are never questions.
Result<Answers> complete_answers(const ResolvedProfile& profile, const Answers& given, const Answers& builtins = {});

// Coerces text to `q`'s type ("5" -> 5, "yes" -> true, "a, b" -> list).
// Tables are not typed at a prompt.
Result<Value> parse_answer(const Question& q, std::string_view text);

// An answers file: a TOML table of id = value (tables as arrays of tables).
Result<Answers> answers_from_toml(std::string_view text, std::string_view name);

// "id=value" from the command line, as text for parse_answer.
Result<std::pair<std::string, std::string>> parse_assignment(std::string_view text);

}  // namespace pychron::setup
