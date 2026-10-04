#pragma once

// The lab's entry settings (sample and package entry spec, section 7, E12):
// one `document` reference, so every client behaves the same way and changes
// are revisioned. Every key has a default.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

inline constexpr std::string_view kSettingsKey = "pychron/entry_settings.json";

struct EntrySettings {
  std::string package_prefix = "NM-";
  std::string default_package_kind = "irradiation";  // irradiation | package
  std::vector<std::string> pi_names_allowed;
  std::string monitor_sample = "FC-2";
  std::string monitor_material = "sanidine";
  std::string irradiation_project_prefix = "Irradiation-";
  bool create_irradiation_project = true;
  double j_multiplier = 1e-4;  // estimated J per hour of dose
  std::string null_identifier_rows = "allow";  // allow | packet (a position without an identifier needs a packet)
  std::string other_json = "{}";  // keys this version does not know, kept as they were
  friend bool operator==(const EntrySettings&, const EntrySettings&) = default;
};

// Missing keys take their defaults; a key of the wrong type is an error.
Result<EntrySettings> parse_settings(std::string_view json);
std::string to_json(const EntrySettings& settings);

struct LoadedSettings {
  EntrySettings settings;
  std::optional<persistence::Uuid> ref_object;  // nullopt: never saved
  std::optional<persistence::Uuid> head;
};

Result<LoadedSettings> load_settings(persistence::IStore& store);
// A new revision on top of `loaded.head` (the reference object is made on the
// first save). A Conflict outcome when someone saved in between.
Result<persistence::CommitOutcome> save_settings(persistence::IStore& store, const persistence::Actor& actor,
                                                 const EntrySettings& settings, const LoadedSettings& loaded);

}  // namespace pychron::entry
