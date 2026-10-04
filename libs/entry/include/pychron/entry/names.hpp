#pragma once

// Name rules and generated names for entry (sample and package entry spec,
// section 6, names.hpp). Every name is trimmed of surrounding whitespace
// before it is checked.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::entry {

std::string trim(std::string_view text);

struct PiName {
  std::string last_name;
  std::string first_initial;  // "" when none
  friend bool operator==(const PiName&, const PiName&) = default;
};

// "Ross", "Ross, J" or "Ross,J" (a capitalised last name, then an optional
// one-letter initial), or one of the lab's `allowed` names taken as a last
// name ("NMGRL"). "Jake Ross" and "ross" are errors (legacy
// sample_entry.py:60-83).
Result<PiName> parse_pi(std::string_view text, const std::vector<std::string>& allowed = {});
// "Last, F" or "Last", as the database's display_name.
std::string display_name(const PiName& name);

// A letter, then letters, digits, '-' or '_'.
bool valid_project_name(std::string_view name);
// Not empty, no whitespace (legacy SpacelessStr).
bool valid_package_name(std::string_view name);

// The next package name with `prefix`: the largest number among `existing`
// names "<prefix><digits>" plus one, zero-padded to that name's width and at
// least 3 digits ("NM-001" -> "NM-002", "NM-999" -> "NM-1000"); "<prefix>001"
// when there is none (legacy labnumber_entry.py:792-870).
std::string next_package_name(const std::vector<std::string>& existing, std::string_view prefix);

// Level letters: "A" -> "B", "Z" -> "AA", "AZ" -> "BA". Empty or not all
// upper-case letters: "A".
std::string next_letters(std::string_view letters);
// The level after the highest letter name among `existing` (shorter first,
// then alphabetical); "A" when there is none.
std::string next_level_name(const std::vector<std::string>& existing);

// Optional letters then digits ("P7", "12").
bool valid_packet(std::string_view packet);
// The packet after `packet`, keeping its letters and digit width:
// "P7" -> "P8", "P09" -> "P10", "9" -> "10". Nullopt for an invalid packet.
std::optional<std::string> next_packet(std::string_view packet);

}  // namespace pychron::entry
