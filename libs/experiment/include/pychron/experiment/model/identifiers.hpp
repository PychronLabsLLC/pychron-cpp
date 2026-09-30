#pragma once

#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"

namespace pychron::experiment {

enum class AnalysisType {
  Unknown, BlankUnknown, BlankAir, BlankCocktail, BlankExtractionLine, Background,
  Air, Cocktail, Pause, Degas, DetectorIC
};

std::string_view to_string(AnalysisType t) noexcept;  // "blank_unknown", ...
std::optional<AnalysisType> parse_analysis_type(std::string_view name);

// Driven by identifiers.toml:
//   [prefixes]            # analysis type name -> special identifier (case-insensitive)
//   blank_unknown = "bu"
//   [patterns]            # optional
//   unknown = "^[A-Za-z0-9][A-Za-z0-9_-]*$"
//   step = "^[A-Za-z]{0,2}$"
class IdentifierRules {
 public:
  static IdentifierRules defaults();  // u b ba bc bu be bg c a pa dg ic
  static Result<IdentifierRules> from_toml(std::string_view text, std::string_view name = "identifiers.toml");
  static Result<IdentifierRules> load(const std::string& path);

  // Exact (case-insensitive) special-identifier match, else Unknown.
  AnalysisType classify(std::string_view identifier) const;
  bool is_special(std::string_view identifier) const;
  // Special identifiers must match a prefix exactly; unknown ones the unknown pattern.
  Result<void> validate_identifier(std::string_view identifier) const;
  bool valid_step(std::string_view step) const;
  std::string prefix_for(AnalysisType t) const;  // "" for Unknown

 private:
  std::map<std::string, AnalysisType> by_prefix_;  // lower-case
  std::regex unknown_re_;
  std::regex step_re_;
};

}  // namespace pychron::experiment
