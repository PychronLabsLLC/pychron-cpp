#include "pychron/earthbank/vocab.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pychron::earthbank {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kUncertaintyUnit = "/api/arar/LUncertaintyUnit";
constexpr std::string_view kMaterials = "/api/core/materials";

// Legacy LOOKUP_ENDPOINTS, entry for entry. `true` marks a list short enough
// to be read whole (LookupSpec::whole): those of the templates' columns.
constexpr std::array kLookupSpecs = {
    LookupSpec{"analysisScaleName", "analysisScaleId", "/api/arar/LAnalysisScale", true},
    LookupSpec{"analyticalUncertaintyUnitName", "analyticalUncertaintyUnitId", kUncertaintyUnit, true},
    LookupSpec{"apparentAgeUncertaintyUnitName", "apparentAgeUncertaintyUnitId", kUncertaintyUnit, true},
    LookupSpec{"jvalueUncertaintyUnitName", "jvalueUncertaintyUnitId", kUncertaintyUnit, true},
    LookupSpec{"arMethodName", "arMethodId", "/api/arar/LArArMethod", true},
    LookupSpec{"ageCalcTypeName", "ageCalcTypeId", "/api/arar/LAgeType", true},
    LookupSpec{"calculatedAgeUncertaintyTypeUnitsName", "calculatedAgeUncertaintyTypeUnitsId",
               kUncertaintyUnit, true},
    LookupSpec{"dataInterpretationToolName", "dataInterpretationToolId",
               "/api/arar/LArArDataInterpretationTool", true},
    LookupSpec{"interpretationName", "interpretationId", "/api/arar/LArArInterpretation", true},
    LookupSpec{"decayConstantName", "decayConstantId", "/api/arar/DecayConstant", true},
    LookupSpec{"airRatioName", "airRatioId", "/api/arar/AirRatio", true},
    LookupSpec{"fluxMonitorName", "fluxMonitorId", "/api/arar/FluxMonitor", true},
    LookupSpec{"mineralName", "mineralId", kMaterials},
    // Yes, No or N/A, kept by the server as an enumeration.
    LookupSpec{"plateauStepName", "plateauStepId", ""},
    LookupSpec{"inverseIsoStepName", "inverseIsoStepId", ""},
    LookupSpec{"weightedMeanStepName", "weightedMeanStepId", ""},
    // The sample.
    LookupSpec{"sampleKindName", "sampleKindId", "/api/core/l-sample-kinds", true},
    LookupSpec{"sampleMethodName", "sampleMethodId", "/api/core/l-sample-methods", true},
    LookupSpec{"materialName", "materialId", kMaterials},
    LookupSpec{"locationKindName", "locationKindId", "/api/core/l-location-kinds", true},
    LookupSpec{"archiveName", "archiveId", "/api/core/archives", true},
    LookupSpec{"stratographicUnitName", "stratographicUnitId", "/api/core/stratigraphic-units"},
    // Not fields of the data point's record: the ids go into link tables.
    LookupSpec{"fundingName", "fundingId", "/api/core/fundings"},
    LookupSpec{"literatureName", "literatureId", "/api/core/literature"},
};

// Legacy STATIC_LOOKUPS.
constexpr std::array kUncertaintyTypeSpecs = {
    LookupSpec{"analyticalUncertaintyTypeName", "analyticalUncertaintyTypeId", ""},
    LookupSpec{"apparentAgeUncertaintyTypeName", "apparentAgeUncertaintyTypeId", ""},
    LookupSpec{"apparentAgeUncertainityTypeName", "apparentAgeUncertainityTypeId", ""},
    LookupSpec{"jvalueUncertaintyTypeName", "jvalueUncertaintyTypeId", ""},
    LookupSpec{"ageUncertaintyTypeName", "ageUncertaintyTypeId", ""},
    LookupSpec{"calculatedAgeUncertaintyTypeName", "calculatedAgeUncertaintyTypeId", ""},
};

struct NamedValue {
  double value;
  std::string_view name;
};

// Total 40K decay constants, per year. The service lists a fourth, `Carter
// et al. 2025`, which no preset of pychron-cpp uses; it is reached by name
// (PublishOptions), not by value.
constexpr std::array kDecayConstants = {
    NamedValue{5.543e-10, "Steiger and Jager, 1977"},
    NamedValue{5.463e-10, "Min et al. 2000"},
    NamedValue{5.5305e-10, "Renne et al. 2011"},  // the sheet's entry ends in a space
};

constexpr std::array kAirRatios = {
    NamedValue{298.56, "Lee et al. 2006"},
    NamedValue{295.5, "Nier 1950"},
};

constexpr double kRelativeTolerance = 1e-4;

std::string name_of(std::span<const NamedValue> table, double value) {
  for (const NamedValue& entry : table) {
    if (std::abs(value - entry.value) <= kRelativeTolerance * entry.value) return std::string(entry.name);
  }
  return {};  // also a NaN
}

struct Citation {
  std::string_view key, name;  // key: "<first author, lower case>_<year>"
};

constexpr std::array kDecayCitations = {
    Citation{"steiger_1977", "Steiger and Jager, 1977"},
    Citation{"steigerjager_1977", "Steiger and Jager, 1977"},
    Citation{"min_2000", "Min et al. 2000"},
    // The citation pychron carries is the 2008 paper, which revises the 2000 value.
    Citation{"min_2008", "Min et al. 2000"},
    Citation{"renne_2010", "Renne et al. 2011"},
    Citation{"renne_2011", "Renne et al. 2011"},
};

constexpr std::array kAirCitations = {
    Citation{"nier_1950", "Nier 1950"},
    Citation{"lee_2006", "Lee et al. 2006"},
};

bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// The first 19xx or 20xx anywhere in the text.
std::string_view first_year(std::string_view text) {
  for (std::size_t i = 0; i + 4 <= text.size(); ++i) {
    const std::string_view century = text.substr(i, 2);
    if ((century == "19" || century == "20") && is_digit(text[i + 2]) && is_digit(text[i + 3])) {
      return text.substr(i, 4);
    }
  }
  return {};
}

// The first run of letters outside parentheses, lower case. A parenthesis
// that is never closed hides nothing.
std::string first_author(std::string_view text) {
  std::string author;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '(') {
      const std::size_t close = text.find(')', i);
      if (close != std::string_view::npos) {
        if (!author.empty()) break;
        i = close + 1;
        continue;
      }
    }
    if (is_alpha(c)) {
      author.push_back(lower(c));
    } else if (!author.empty()) {
      break;
    }
    ++i;
  }
  return author;
}

// "author_year", or "author" with no year; empty with no author.
std::string citation_key(std::string_view citation) {
  std::string key = first_author(citation);
  if (key.empty()) return key;
  if (const std::string_view year = first_year(citation); !year.empty()) {
    key.push_back('_');
    key.append(year);
  }
  return key;
}

std::string shortest(double value) {
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string{};
}

}  // namespace

std::span<const LookupSpec> lookup_specs() { return kLookupSpecs; }

std::span<const LookupSpec> uncertainty_type_specs() { return kUncertaintyTypeSpecs; }

std::optional<int> uncertainty_type_id(std::string_view name) {
  std::string key;
  for (const char c : name) {
    if (is_alpha(c) || is_digit(c)) key.push_back(lower(c));
  }
  for (const std::string_view one : {"1sigma"sv, "1s"sv, "1"sv, "se"sv, "sem"sv, "sd"sv, "msem"sv}) {
    if (key == one) return 1;
  }
  for (const std::string_view two : {"2sigma"sv, "2s"sv, "2"sv}) {
    if (key == two) return 2;
  }
  return std::nullopt;
}

std::string ar_method(bool any_step, std::optional<double> max_extract_value) {
  if (!any_step) return "Total fusion - laser";
  if (max_extract_value) {
    if (*max_extract_value >= 300.0) return "Step-heating - furnace";
    if (*max_extract_value <= 50.0) return "Step-heating - laser";
  }
  return "Step-heating - undefined";
}

std::string decay_constant_name(double lambda_total_per_year) {
  return name_of(kDecayConstants, lambda_total_per_year);
}

std::string air_ratio_name(double atm4036) { return name_of(kAirRatios, atm4036); }

std::string normalize_citation(std::string_view override_text, bool decay) {
  const std::string key = citation_key(override_text);
  const std::span<const Citation> table =
      decay ? std::span<const Citation>(kDecayCitations) : std::span<const Citation>(kAirCitations);
  for (const Citation& citation : table) {
    if (key == citation.key) return std::string(citation.name);
  }
  return std::string(override_text);
}

std::string flux_monitor_query(std::string_view name, std::optional<double> age_ma) {
  std::string query(name);
  if (age_ma) {
    if (!query.empty()) query.push_back(' ');
    query += shortest(*age_ma);
    query += " Ma";
  }
  return query;
}

// ---------------------------------------------------------------- lists

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool ascii_digit(char c) { return c >= '0' && c <= '9'; }

// The same text but for the case of ASCII letters.
bool same_but_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

// The names pychron labs use for the monitors of the template's `Flux
// Monitor` list, each with the mark of that monitor's entries there:
//   (ACs)      Alder Creek Rhyolite Sanidine
//   (FCTs)     Fish Canyon Tuff Sanidine
//   (Hb3gr)    Hornblende
//   (GA-1550)  Mt Domedary Biotite (sic)
//   (MD-2)     Mt Domedary Biotite (sic), another split
//   (TCs)      Taylor Creek Rhyolite Saniditine (sic)
//   WA1ms      (Western Australia Muscovite)
// `Mt Dromedary` alone is not here: it is two monitors of the list.
constexpr MonitorAlias kMonitorAliases[] = {
    {"FC-2", "FCTs"},    {"FC2", "FCTs"},         {"FCs", "FCTs"},      {"FCT", "FCTs"},  {"FCTs", "FCTs"},
    {"Fish Canyon", "FCTs"},
    {"ACs", "ACs"},      {"Alder Creek", "ACs"},
    {"GA-1550", "GA-1550"}, {"GA1550", "GA-1550"},
    {"MD-2", "MD-2"},    {"MD2", "MD-2"},
    {"Hb3gr", "Hb3gr"},
    {"TCs", "TCs"},      {"TCR", "TCs"},          {"Taylor Creek", "TCs"},
    {"WA1ms", "WA1ms"},
};

// Designations that hold one of the names above and are another monitor, one
// the list has not: FCT-3 is the biotite of the Fish Canyon Tuff, and `FCT`
// would make it the sanidine. A name that holds one matches nothing.
constexpr std::string_view kNotListed[] = {"FCT-3", "FCT3"};

// The words of the list's entries that are minerals, and the mineral each
// is. `Saniditine` is the list's spelling in its Taylor Creek entry. A name
// that says one of these is of an entry that says the same one, and of no
// other: the same tuff has a sanidine and a biotite, and the list only one.
constexpr MonitorMineral kMonitorMinerals[] = {
    {"sanidine", "sanidine"},     {"saniditine", "sanidine"}, {"biotite", "biotite"},
    {"hornblende", "hornblende"}, {"muscovite", "muscovite"},
};

// Words a lab writes for the material of a monitor that are not words of the
// list: the short forms of the list's four minerals, which are those
// minerals, and materials the list has no monitor of, which are no entry's.
// `K-feldspar` is `k`, `feldspar` as words and is told by the second, and
// `K-spar` by `spar`. A term of two words is told by the two together
// (`whole rock`, `whole-rock`).
// None of these is a word of an alias or of an entry (a test holds that).
constexpr MonitorMineral kOtherMaterials[] = {
    {"san", "sanidine"},          {"bt", "biotite"},         {"bio", "biotite"},
    {"hbl", "hornblende"},        {"ms", "muscovite"},       {"musc", "muscovite"},
    {"plagioclase", "plagioclase"}, {"plag", "plagioclase"},
    {"feldspar", "feldspar"},     {"kfeldspar", "feldspar"}, {"kspar", "feldspar"},   {"kfs", "feldspar"},
    {"spar", "feldspar"},
    {"orthoclase", "orthoclase"}, {"anorthoclase", "anorthoclase"},
    {"adularia", "adularia"},     {"microcline", "microcline"},
    {"amphibole", "amphibole"},   {"phlogopite", "phlogopite"},
    {"sericite", "sericite"},     {"glauconite", "glauconite"},
    {"alunite", "alunite"},       {"jarosite", "jarosite"},
    {"glass", "glass"},           {"groundmass", "groundmass"},
    {"whole rock", "whole rock"}, {"wholerock", "whole rock"},
};

// The words of `text`: runs of ASCII letters and digits (and of other bytes
// that are not ASCII), lower case. `FC-2` is `fc`, `2`.
std::vector<std::string> words_of(std::string_view text) {
  std::vector<std::string> out;
  std::string word;
  for (const char c : text) {
    const bool part = ascii_letter(c) || ascii_digit(c) || static_cast<unsigned char>(c) >= 0x80;
    if (part) {
      word += ascii_lower(c);
    } else if (!word.empty()) {
      out.push_back(std::move(word));
      word.clear();
    }
  }
  if (!word.empty()) out.push_back(std::move(word));
  return out;
}

// `part` is in `all` as a run of consecutive words.
bool has_run(const std::vector<std::string>& all, const std::vector<std::string>& part) {
  if (part.empty() || part.size() > all.size()) return false;
  for (std::size_t i = 0; i + part.size() <= all.size(); ++i) {
    if (std::equal(part.begin(), part.end(), all.begin() + static_cast<std::ptrdiff_t>(i))) return true;
  }
  return false;
}

// `words` say `term` (one word or a run of them), in the singular or with an
// `s` after it: `biotites` are biotite.
bool says(const std::vector<std::string>& words, std::string_view term) {
  std::vector<std::string> run = words_of(term);
  if (has_run(words, run)) return true;
  if (run.empty()) return false;
  run.back() += 's';
  return has_run(words, run);
}

// The minerals and other materials `words` say, each once, sorted.
std::vector<std::string_view> minerals_of(const std::vector<std::string>& words) {
  std::vector<std::string_view> out;
  for (const std::span<const MonitorMineral> table :
       {std::span<const MonitorMineral>(kMonitorMinerals), std::span<const MonitorMineral>(kOtherMaterials)}) {
    for (const MonitorMineral& mineral : table) {
      if (!says(words, mineral.word)) continue;
      if (std::find(out.begin(), out.end(), mineral.mineral) == out.end()) out.push_back(mineral.mineral);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// A number as people write an age (`28.201`, `1081`), so that two are
// compared as numbers without a rounding: no zeros before it or after its
// fraction. Empty when `text` is not one.
std::string plain_number(std::string_view text) {
  const std::size_t dot = text.find('.');
  std::string_view whole = text.substr(0, dot);
  std::string_view fraction = dot == std::string_view::npos ? std::string_view() : text.substr(dot + 1);
  if (whole.empty() || !std::all_of(whole.begin(), whole.end(), ascii_digit)) return {};
  if (!std::all_of(fraction.begin(), fraction.end(), ascii_digit)) return {};
  if (dot != std::string_view::npos && fraction.empty()) return {};
  while (whole.size() > 1 && whole.front() == '0') whole.remove_prefix(1);
  while (!fraction.empty() && fraction.back() == '0') fraction.remove_suffix(1);
  std::string out(whole);
  if (!fraction.empty()) out += "." + std::string(fraction);
  return out;
}

// A monitor as a lab writes it, taken apart: its age and the words of the
// rest.
struct MonitorQuery {
  std::string age;       // plain_number; empty: none given
  bool age_in_ma = false;  // the value says so (`28.201 Ma`); else it is the last number
  std::vector<std::string> name;
};

MonitorQuery monitor_query(std::string_view value) {
  // Split at spaces; each piece less the punctuation around it.
  struct Piece {
    std::string_view text;  // as written, for the name
    std::string number;     // when it is a number, or a number run on with `Ma`
    bool is_ma = false;
    bool has_ma = false;
  };
  std::vector<Piece> pieces;
  std::string_view rest = value;
  while (true) {
    rest = trimmed(rest);
    if (rest.empty()) break;
    std::size_t end = 0;
    while (end < rest.size() && !is_blank(rest[end])) ++end;
    Piece piece{rest.substr(0, end), {}, false, false};
    rest.remove_prefix(end);
    std::string_view core = piece.text;
    const auto edge = [](char c) { return !ascii_letter(c) && !ascii_digit(c) && static_cast<unsigned char>(c) < 0x80; };
    while (!core.empty() && edge(core.front())) core.remove_prefix(1);
    while (!core.empty() && edge(core.back())) core.remove_suffix(1);
    piece.is_ma = same_but_case(core, "ma");
    piece.number = plain_number(core);
    if (piece.number.empty() && core.size() > 2 && same_but_case(core.substr(core.size() - 2), "ma")) {
      piece.number = plain_number(core.substr(0, core.size() - 2));
      piece.has_ma = !piece.number.empty();
    }
    pieces.push_back(std::move(piece));
  }

  // The number said to be in Ma, else the last number.
  std::optional<std::size_t> at;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (pieces[i].number.empty()) continue;
    if (pieces[i].has_ma || (i + 1 < pieces.size() && pieces[i + 1].is_ma)) {
      at = i;
      break;
    }
  }
  MonitorQuery out;
  out.age_in_ma = at.has_value();
  if (!at) {
    for (std::size_t i = pieces.size(); i-- > 0;) {
      if (pieces[i].number.empty()) continue;
      at = i;
      break;
    }
  }
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (at && i == *at) {
      out.age = pieces[i].number;
      continue;
    }
    if (at && i == *at + 1 && pieces[i].is_ma) continue;
    for (std::string& word : words_of(pieces[i].text)) out.name.push_back(std::move(word));
  }
  return out;
}

// The age an entry of the list ends with, `(28.201 Ma)`; empty when it has
// none.
std::string listed_age(std::string_view entry) {
  entry = trimmed(entry);
  constexpr std::string_view kTail = " Ma)";
  if (!entry.ends_with(kTail)) return {};
  entry.remove_suffix(kTail.size());
  const std::size_t open = entry.rfind('(');
  return open == std::string_view::npos ? std::string() : plain_number(trimmed(entry.substr(open + 1)));
}

// An entry less the age it ends with: what is left names the monitor and
// the paper.
std::string_view without_listed_age(std::string_view entry) {
  if (listed_age(entry).empty()) return entry;
  return trimmed(entry.substr(0, entry.rfind('(')));
}

// A number that reads as the year of a paper.
bool is_year(std::string_view number) {
  return number.size() == 4 && (number.starts_with("19") || number.starts_with("20")) &&
         std::all_of(number.begin(), number.end(), ascii_digit);
}

// `word` is one of the words for a mineral or another material, or its plural.
bool is_material_word(std::string_view word) {
  for (const std::span<const MonitorMineral> table :
       {std::span<const MonitorMineral>(kMonitorMinerals), std::span<const MonitorMineral>(kOtherMaterials)}) {
    for (const MonitorMineral& mineral : table) {
      if (word == mineral.word) return true;
      if (word.size() == mineral.word.size() + 1 && word.back() == 's' && word.starts_with(mineral.word)) return true;
    }
  }
  return false;
}

// `words` less every run of them that is `part`.
std::vector<std::string> without_run(const std::vector<std::string>& words, const std::vector<std::string>& part) {
  if (part.empty()) return words;
  std::vector<std::string> out;
  for (std::size_t i = 0; i < words.size();) {
    if (i + part.size() <= words.size() &&
        std::equal(part.begin(), part.end(), words.begin() + static_cast<std::ptrdiff_t>(i))) {
      i += part.size();
    } else {
      out.push_back(words[i++]);
    }
  }
  return out;
}

bool has_word(const std::vector<std::string>& words, std::string_view word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

// The abbreviations an entry has in parentheses, as words: `(GA-1550)` is
// `ga`, `1550`. A year in parentheses is not one.
std::vector<std::vector<std::string>> abbreviations_of(std::string_view entry) {
  std::vector<std::vector<std::string>> out;
  for (std::size_t open = entry.find('('); open != std::string_view::npos; open = entry.find('(', open + 1)) {
    const std::size_t close = entry.find(')', open);
    if (close == std::string_view::npos) break;
    const std::string_view inside = entry.substr(open + 1, close - open - 1);
    if (inside.empty() || std::any_of(inside.begin(), inside.end(), is_blank)) continue;
    if (!std::any_of(inside.begin(), inside.end(), ascii_letter)) continue;
    out.push_back(words_of(inside));
  }
  return out;
}

}  // namespace

std::span<const MonitorAlias> flux_monitor_aliases() { return kMonitorAliases; }

std::span<const MonitorMineral> flux_monitor_minerals() { return kMonitorMinerals; }
std::span<const MonitorMineral> flux_monitor_other_materials() { return kOtherMaterials; }

ListMatch match_list_entry(std::string_view value, std::span<const std::string> entries) {
  const std::string_view wanted = trimmed(value);
  if (wanted.empty()) return {};
  // The entries that are the value but for case and the spaces around, each
  // spelling once.
  std::vector<std::string_view> spellings;
  for (const std::string& entry : entries) {
    if (entry == value) return {ListMatch::Kind::Exact, std::string(value)};
    const std::string_view listed = trimmed(entry);
    if (!same_but_case(listed, wanted)) continue;
    if (std::find(spellings.begin(), spellings.end(), listed) == spellings.end()) spellings.push_back(listed);
  }
  if (spellings.empty()) return {};
  // Among several, the one that is the value's own case is the one meant.
  std::string_view chosen = spellings.front();
  if (spellings.size() > 1) {
    const auto own = std::find(spellings.begin(), spellings.end(), wanted);
    if (own == spellings.end()) return {ListMatch::Kind::Ambiguous, {}};
    chosen = *own;
  }
  return {chosen == value ? ListMatch::Kind::Exact : ListMatch::Kind::Respelled, std::string(chosen)};
}

ListMatch match_flux_monitor(std::string_view value, std::span<const std::string> entries) {
  if (ListMatch plain = match_list_entry(value, entries); plain.kind != ListMatch::Kind::None) return plain;

  const MonitorQuery query = monitor_query(value);
  if (query.age.empty() || query.name.empty()) return {};

  // The monitors the name names, by the mark of their entries, and the words
  // of the name that are not such a name.
  std::vector<std::vector<std::string>> marks;
  std::vector<std::string> beside = query.name;
  for (const MonitorAlias& alias : kMonitorAliases) {
    const std::vector<std::string> said = words_of(alias.alias);
    if (!has_run(query.name, said)) continue;
    beside = without_run(beside, said);
    std::vector<std::string> mark = words_of(alias.listed);
    if (std::find(marks.begin(), marks.end(), mark) == marks.end()) marks.push_back(std::move(mark));
  }
  if (marks.size() > 1) return {};
  for (const std::string_view other : kNotListed) {
    if (has_run(query.name, words_of(other))) return {};
  }
  const std::vector<std::string_view> said = minerals_of(query.name);
  std::erase_if(beside, [](const std::string& word) { return is_material_word(word); });

  // What the papers of a monitor say: the words of each of its entries, less
  // the entry's age. A word beside the monitor's name that is one of these
  // tells its entries apart, and must be the found entry's.
  std::vector<std::string> of_the_monitor;
  if (!marks.empty()) {
    for (const std::string& entry : entries) {
      if (!has_run(words_of(entry), marks.front())) continue;
      for (std::string& word : words_of(without_listed_age(entry))) of_the_monitor.push_back(std::move(word));
    }
  }
  const auto telling = [](const std::string& word) {
    return word.size() >= 3 && std::all_of(word.begin(), word.end(), ascii_letter);
  };

  // By the age, and when no entry is of it and the number was not said to be
  // an age, by the paper it is the year of.
  const bool may_be_a_year = !query.age_in_ma && is_year(query.age);
  for (const bool by_paper : {false, true}) {
    if (by_paper && !may_be_a_year) break;
    std::vector<std::string_view> found;
    for (const std::string& entry : entries) {
      const std::vector<std::string> words = words_of(entry);
      // The words a name or a paper is found among: by the paper, not the
      // digits of the entry's age.
      const std::vector<std::string> paper = words_of(without_listed_age(entry));
      const std::vector<std::string>& cited = by_paper ? paper : words;
      if (by_paper ? !has_word(cited, query.age) : listed_age(entry) != query.age) continue;
      // Another mineral or material is another monitor, whatever else of the
      // name fits.
      if (const auto listed = minerals_of(words); !said.empty() && !listed.empty() && said != listed) continue;
      bool named = false;
      if (!marks.empty()) {
        named = has_run(words, marks.front());
        if (by_paper) {
          // By the paper the monitor is not enough: the author must be there,
          // and nothing said that the entry does not say.
          named = named && std::any_of(beside.begin(), beside.end(), telling) &&
                  std::all_of(beside.begin(), beside.end(), [&](const std::string& word) { return has_word(cited, word); });
        } else {
          // By the age, a paper said beside it must be this entry's: a year,
          // or a word of another entry of the monitor (its author), that this
          // entry has not makes the value say two things, and it is refused
          // rather than settled by the age.
          named = named && std::none_of(beside.begin(), beside.end(), [&](const std::string& word) {
            return (is_year(word) || has_word(of_the_monitor, word)) && !has_word(paper, word);
          });
        }
      } else if (by_paper) {
        // No name this knows, and no age: the value must name the monitor by
        // the abbreviation the entry has in parentheses. An author and a
        // year, a mineral and a paper, or words of the entry say which paper
        // and not which monitor, though one entry only be of it. Beside the
        // abbreviation the author is wanted, and no word the entry has not.
        for (const auto& abbreviation : abbreviations_of(entry)) {
          if (!has_run(query.name, abbreviation)) continue;
          std::vector<std::string> rest = without_run(query.name, abbreviation);
          std::erase_if(rest, [](const std::string& word) { return is_material_word(word); });
          named = named || (std::any_of(rest.begin(), rest.end(), telling) &&
                            std::all_of(query.name.begin(), query.name.end(),
                                        [&](const std::string& word) { return has_word(cited, word); }));
        }
      } else {
        // No name this knows: every word given must be the entry's, and one of
        // them must be a name and not a year or a conjunction's worth of
        // letters.
        const bool all = std::all_of(query.name.begin(), query.name.end(),
                                     [&](const std::string& word) { return has_word(cited, word); });
        bool tells = std::any_of(query.name.begin(), query.name.end(), telling);
        for (const auto& abbreviation : abbreviations_of(entry)) tells = tells || has_run(query.name, abbreviation);
        named = all && tells;
      }
      if (!named) continue;
      const std::string_view listed = trimmed(entry);
      if (std::find(found.begin(), found.end(), listed) == found.end()) found.push_back(listed);
    }
    if (found.size() > 1) return {ListMatch::Kind::Ambiguous, {}};
    if (found.size() == 1) return {ListMatch::Kind::Respelled, std::string(found.front())};
  }
  return {};
}

std::string_view age_type_name(AgeKind kind) {
  switch (kind) {
    case AgeKind::Plateau: return "Plateau Age";
    case AgeKind::WeightedMean: return "Weighted Mean Age";
    case AgeKind::Isochron: return "Inverse Isochron Age";
    // The integrated age: all the gas of a heating schedule summed.
    case AgeKind::Integrated: return "Total Gas Age";
  }
  return "";
}

}  // namespace pychron::earthbank
