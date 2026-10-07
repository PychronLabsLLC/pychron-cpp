#include "pychron/experiment/model/identifiers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include <toml++/toml.hpp>

namespace pychron::experiment {
namespace {

struct TypeName {
  AnalysisType type;
  std::string_view name;
  std::string_view default_prefix;  // "" = none
};

constexpr std::array<TypeName, 11> kTypes{{
    {AnalysisType::Unknown, "unknown", "u"},
    {AnalysisType::BlankUnknown, "blank_unknown", "bu"},
    {AnalysisType::BlankAir, "blank_air", "ba"},
    {AnalysisType::BlankCocktail, "blank_cocktail", "bc"},
    {AnalysisType::BlankExtractionLine, "blank_extractionline", "be"},
    {AnalysisType::Background, "background", "bg"},
    {AnalysisType::Air, "air", "a"},
    {AnalysisType::Cocktail, "cocktail", "c"},
    {AnalysisType::Pause, "pause", "pa"},
    {AnalysisType::Degas, "degas", "dg"},
    {AnalysisType::DetectorIC, "detector_ic", "ic"},
}};

constexpr std::string_view kDefaultUnknown = "^[A-Za-z0-9][A-Za-z0-9_-]*$";
constexpr std::string_view kDefaultStep = "^[A-Za-z]{0,2}$";

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

}  // namespace

std::string_view to_string(AnalysisType t) noexcept {
  for (const auto& e : kTypes)
    if (e.type == t) return e.name;
  return "unknown";
}

std::optional<AnalysisType> parse_analysis_type(std::string_view name) {
  for (const auto& e : kTypes)
    if (e.name == name) return e.type;
  return std::nullopt;
}

IdentifierRules IdentifierRules::defaults() {
  IdentifierRules r;
  for (const auto& e : kTypes) r.by_prefix_.emplace(std::string(e.default_prefix), e.type);
  // Pychron treats bare "b" as an unspecified blank, i.e. blank_unknown.
  r.by_prefix_.emplace("b", AnalysisType::BlankUnknown);
  r.unknown_re_ = std::regex(std::string(kDefaultUnknown));
  r.step_re_ = std::regex(std::string(kDefaultStep));
  return r;
}

Result<IdentifierRules> IdentifierRules::from_toml(std::string_view text, std::string_view name) {
  auto parsed = toml::parse(text, name);
  if (!parsed) {
    return fail(ErrorKind::Config, std::string(name) + ": syntax error: " + std::string(parsed.error().description()));
  }
  const toml::table& root = parsed.table();
  IdentifierRules r;
  std::string unknown_pat(kDefaultUnknown), step_pat(kDefaultStep);

  if (const auto* prefixes = root["prefixes"].as_table()) {
    for (const auto& [key, node] : *prefixes) {
      auto type = parse_analysis_type(key.str());
      if (!type) return fail(ErrorKind::Config, std::string(name) + ": unknown analysis type '" + std::string(key.str()) + "'");
      // One identifier, or several: ["bu", "b"].
      std::vector<std::string> given;
      bool bad = false;
      if (const auto* list = node.as_array()) {
        for (const auto& item : *list) {
          auto s = item.value<std::string>();
          if (!s || s->empty()) bad = true;
          else given.push_back(*s);
        }
      } else if (auto s = node.value<std::string>(); s && !s->empty()) {
        given.push_back(*s);
      }
      if (bad || given.empty())
        return fail(ErrorKind::Config, std::string(name) + ": prefix for '" + std::string(key.str()) +
                                           "' must be a non-empty string, or a list of them");
      for (const auto& s : given) {
        auto [it, ok] = r.by_prefix_.emplace(lower(s), *type);
        if (!ok) return fail(ErrorKind::Config, std::string(name) + ": duplicate prefix '" + s + "'");
      }
    }
  } else if (root.contains("prefixes")) {
    return fail(ErrorKind::Config, std::string(name) + ": 'prefixes' must be a table");
  }
  if (r.by_prefix_.empty()) return fail(ErrorKind::Config, std::string(name) + ": no [prefixes] defined");

  if (const auto* patterns = root["patterns"].as_table()) {
    for (const auto& [key, node] : *patterns) {
      auto s = node.value<std::string>();
      if (!s) return fail(ErrorKind::Config, std::string(name) + ": pattern '" + std::string(key.str()) + "' must be a string");
      if (key.str() == "unknown") unknown_pat = *s;
      else if (key.str() == "step") step_pat = *s;
      else return fail(ErrorKind::Config, std::string(name) + ": unknown pattern '" + std::string(key.str()) + "'");
    }
  }
  try {
    r.unknown_re_ = std::regex(unknown_pat);
    r.step_re_ = std::regex(step_pat);
  } catch (const std::regex_error& e) {
    return fail(ErrorKind::Config, std::string(name) + ": bad regex: " + e.what());
  }
  return r;
}

Result<IdentifierRules> IdentifierRules::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_toml(ss.str(), path);
}

AnalysisType IdentifierRules::classify(std::string_view identifier) const {
  auto it = by_prefix_.find(lower(identifier));
  return it == by_prefix_.end() ? AnalysisType::Unknown : it->second;
}

bool IdentifierRules::is_special(std::string_view identifier) const {
  return by_prefix_.count(lower(identifier)) != 0;
}

Result<void> IdentifierRules::validate_identifier(std::string_view identifier) const {
  if (identifier.empty()) return fail(ErrorKind::Config, "identifier is empty");
  if (is_special(identifier)) return {};
  // Looks like a special identifier with junk attached (e.g. "bu-2") is a typo, not a sample.
  if (!std::regex_match(std::string(identifier), unknown_re_)) {
    return fail(ErrorKind::Config, "identifier '" + std::string(identifier) + "' is malformed");
  }
  return {};
}

bool IdentifierRules::valid_step(std::string_view step) const {
  return std::regex_match(std::string(step), step_re_);
}

std::string IdentifierRules::prefix_for(AnalysisType t) const {
  if (t == AnalysisType::Unknown) return {};
  std::string best;
  for (const auto& [prefix, type] : by_prefix_)
    if (type == t && (best.empty() || prefix.size() > best.size() || (prefix.size() == best.size() && prefix < best))) best = prefix;
  return best;
}

}  // namespace pychron::experiment
