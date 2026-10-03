// The layout of a legacy MetaData repository (meta_layout.hpp).

#include "meta_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

#include "pychron/ingest/tz.hpp"
#include "pychron/persistence/ids.hpp"

namespace pychron::dvc {

namespace ps = pychron::persistence;

namespace {

// ---------------------------------------------------------------- text

std::string_view trim(std::string_view s) {
  constexpr std::string_view kSpace = " \t\r\n\f\v";
  const auto first = s.find_first_not_of(kSpace);
  if (first == std::string_view::npos) return {};
  return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  for (;;) {
    const auto at = text.find(separator);
    parts.push_back(text.substr(0, at));
    if (at == std::string_view::npos) return parts;
    text.remove_prefix(at + 1);
  }
}

// The lines of a text file, without their line ends; a BOM is skipped. A
// final line end does not start another line.
std::vector<std::string_view> lines_of(std::string_view text) {
  if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  std::vector<std::string_view> lines = split(text, '\n');
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  for (auto& line : lines)
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return lines;
}

// A field of a text file as a number: what as_double reads from a string.
std::optional<double> number(std::string_view field) { return as_double(Json(std::string(trim(field)))); }

Json line_note(std::size_t index, std::string_view line) {
  return Json{{"line", index + 1}, {"text", std::string(line)}};
}

// A naive local time as UTC; nullopt when it is not a time. `what` names it
// in the note left when the zone cannot place it uniquely.
std::optional<ps::UtcTime> to_utc(std::string_view naive, std::string_view zone, const std::string& what,
                                  Json& detail) {
  const std::string text(trim(naive));
  const auto converted = ingest::local_to_utc(text, zone);
  if (!converted) return std::nullopt;
  if (converted->kind == ingest::LocalKind::Ambiguous)
    detail["notes"].push_back(what + " " + text + " is ambiguous in " + std::string(zone) +
                              " (clocks set back): the earlier instant was taken");
  if (converted->kind == ingest::LocalKind::Nonexistent)
    detail["notes"].push_back(what + " " + text + " is nonexistent in " + std::string(zone) +
                              " (clocks set forward): the start of the gap was taken");
  return converted->utc;
}

// ---------------------------------------------------------------- paths

bool is_name(std::string_view text) { return !text.empty() && text.front() != '.'; }

// The part of `file` before `suffix` when it is a name; empty otherwise.
std::string_view stem(std::string_view file, std::string_view suffix) {
  if (!file.ends_with(suffix)) return {};
  const std::string_view name = file.substr(0, file.size() - suffix.size());
  return is_name(name) ? name : std::string_view{};
}

bool is_reserved(std::string_view directory) {
  constexpr std::string_view kReserved[] = {"spectrometers", "irradiation_holders", "load_holders",
                                            "productions",   "scripts",             "experiments"};
  return std::find(std::begin(kReserved), std::end(kReserved), directory) != std::end(kReserved);
}

MetaPath named(MetaKind kind, std::string_view irradiation, std::string_view name) {
  if (name.empty() && (kind != MetaKind::LevelProductions && kind != MetaKind::Chronology)) return {};
  return {kind, std::string(irradiation), std::string(name)};
}

// ---------------------------------------------------------------- JSON

// The entries of a JSON list with the tokens under each ("<prefix>/<index>").
std::vector<MetaEntry> entries_of(const Json& list, const std::vector<NonFinite>& nonfinite,
                                  const std::string& prefix) {
  std::vector<MetaEntry> entries;
  entries.reserve(list.size());
  std::size_t index = 0;
  for (const auto& entry : list)
    entries.push_back({entry, nonfinite_under(nonfinite, prefix + "/" + std::to_string(index++))});
  return entries;
}

// Moves the typed fields of an object member into `take`; what is left of
// the member stays in `object`, and nothing when that is nothing.
template <class Take>
void take_from(Json& object, const char* key, Take&& take) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_object()) return;
  take(*it);
  if (it->empty()) object.erase(it);
}

void take_analyses(Json& entry, ps::FluxValue& value) {
  const auto found = entry.find("analyses");
  if (found == entry.end() || !found->is_array()) return;  // not a list: stays in extra as it is
  Json leftovers = Json::object();
  Json unmapped = Json::array();
  std::set<std::string> seen;
  for (auto& item : *found) {
    const auto record_id = item.is_object() ? item.find("record_id") : item.end();
    if (!item.is_object() || record_id == item.end() || !record_id->is_string() ||
        !seen.insert(record_id->get<std::string>()).second) {
      unmapped.push_back(item);
      continue;
    }
    ps::FluxAnalysis analysis;
    analysis.record_id = record_id->get<std::string>();
    Json rest = item;
    rest.erase("record_id");
    if (const auto uuid = rest.find("uuid"); uuid != rest.end() && uuid->is_string()) {
      analysis.analysis = ps::Uuid::parse(uuid->get<std::string>());
      if (analysis.analysis) rest.erase(uuid);
    }
    // Newer files say "is_omitted"; older ones wrote the same flag as "status".
    if (const auto omitted = take_bool(rest, "is_omitted"))
      analysis.is_omitted = *omitted;
    else if (const auto status = take_bool(rest, "status"))
      analysis.is_omitted = *status;
    if (!rest.empty()) leftovers[analysis.record_id] = std::move(rest);
    value.analyses.push_back(std::move(analysis));
  }
  entry.erase(found);
  if (!leftovers.empty()) entry["analyses"] = std::move(leftovers);
  if (!unmapped.empty()) entry["analyses_unmapped"] = std::move(unmapped);
}

}  // namespace

MetaPath classify_meta_path(std::string_view repo_path) {
  const std::vector<std::string_view> parts = split(repo_path, '/');
  if (parts.size() < 2 || parts.size() > 3 || !is_name(parts[0])) return {};
  const std::string_view top = parts[0];
  const std::string_view file = parts.back();

  if (parts.size() == 3) {
    if (is_reserved(top) || parts[1] != "productions") return {};
    return named(MetaKind::Production, top, stem(file, ".json"));
  }
  if (top == "spectrometers") {
    if (const auto name = stem(file, ".gain.json"); !name.empty()) return named(MetaKind::Gains, {}, name);
    return named(MetaKind::Sensitivity, {}, stem(file, ".sens.json"));
  }
  if (top == "irradiation_holders") return named(MetaKind::IrradiationHolder, {}, stem(file, ".txt"));
  if (top == "load_holders") return named(MetaKind::LoadHolder, {}, stem(file, ".txt"));
  if (is_reserved(top)) return {};
  if (file == "chronology.txt") return named(MetaKind::Chronology, top, {});
  if (file == "productions.json") return named(MetaKind::LevelProductions, top, {});
  return named(MetaKind::Level, top, stem(file, ".json"));
}

// ---------------------------------------------------------------- level file

Result<ParsedLevel> parse_level(std::string_view json) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  Json doc = std::move(*parsed);

  ParsedLevel level;
  std::vector<MetaEntry> entries;
  if (doc.is_array()) {
    entries = entries_of(doc, nonfinite, "");
  } else if (doc.is_object()) {
    const auto positions = doc.find("positions");
    if (positions == doc.end() || !positions->is_array())
      return fail(ErrorKind::Protocol, "level file has no \"positions\" list");
    entries = entries_of(*positions, nonfinite, "/positions");
    doc.erase(positions);
    for (const auto& n : nonfinite)
      if (!n.pointer.starts_with("/positions/")) level.header_nonfinite[n.pointer] = n.token;
    level.header = std::move(doc);
  } else {
    return fail(ErrorKind::Protocol, "level file is neither an object nor a list of positions");
  }

  std::size_t index = 0;
  for (auto& entry : entries) {
    const auto position = entry.entry.is_object() ? entry.entry.find("position") : entry.entry.end();
    const auto hole = entry.entry.is_object() && position != entry.entry.end() ? as_int(*position) : std::nullopt;
    if (!hole)
      return fail(ErrorKind::Protocol, "level file positions[" + std::to_string(index) + "] has no hole number");
    level.positions[*hole].push_back(std::move(entry));
    ++index;
  }
  return level;
}

ParsedFlux flux_value(const std::vector<MetaEntry>& entries) {
  ParsedFlux out;
  ps::FluxValue& value = out.value;
  Json entry = entries.front().entry;
  entry.erase("position");  // the object's key
  value.j = take_double(entry, "j");
  value.j_err = take_double(entry, "j_err");
  value.mean_j = take_double(entry, "mean_j");
  value.mean_j_err = take_double(entry, "mean_j_err");
  value.mean_j_mswd = take_double(entry, "mean_j_mswd");
  value.position_jerr = take_double(entry, "position_jerr");
  take_from(entry, "decay_constants", [&](Json& constants) {
    value.lambda_k_total = take_double(constants, "lambda_k_total");
    value.lambda_k_total_err = take_double(constants, "lambda_k_total_error");
  });
  take_from(entry, "monitor", [&](Json& monitor) {
    value.monitor_name = take_text(monitor, "name");
    value.monitor_material = take_text(monitor, "material");
    value.monitor_age = take_double(monitor, "age");
    value.monitor_age_err = take_double(monitor, "error");
  });
  value.options_json = take_json(entry, "options");
  take_analyses(entry, value);
  if (!entries.front().nonfinite.is_null()) entry["nonfinite"] = entries.front().nonfinite;
  value.extra_json = extra_text(entry);

  for (std::size_t i = 1; i < entries.size(); ++i) out.detail["duplicate_entries"].push_back(entries[i].entry);
  return out;
}

ParsedLevelZ level_z_value(const ParsedLevel& level) {
  ParsedLevelZ out;
  Json header = level.header;
  out.value.z = take_double(header, "z");
  if (!header.empty()) out.detail["extra"] = std::move(header);
  if (!level.header_nonfinite.is_null()) out.detail["nonfinite"] = level.header_nonfinite;
  return out;
}

// ---------------------------------------------------------------- productions.json

Result<ParsedLevelProductions> parse_level_productions(std::string_view json) {
  auto parsed = parse_legacy(json);
  if (!parsed) return fail(parsed.error());
  if (!parsed->is_object()) return fail(ErrorKind::Protocol, "productions.json is not a JSON object");
  ParsedLevelProductions out;
  for (auto it = parsed->begin(); it != parsed->end(); ++it) {
    const bool text = it->is_string() && !it->get_ref<const std::string&>().empty();
    if (text && it.key() == "note")
      out.note = it->get<std::string>();
    else if (text)
      out.levels.emplace(it.key(), it->get<std::string>());
    else
      out.extra[it.key()] = *it;
  }
  return out;
}

// ---------------------------------------------------------------- chronology

Result<ParsedChronology> parse_chronology(std::string_view text, std::string_view lab_time_zone) {
  if (!ingest::known_zone(lab_time_zone))
    return fail(ErrorKind::Config, "chronology: unknown time zone '" + std::string(lab_time_zone) + "'");
  ParsedChronology out;
  const auto lines = lines_of(text);
  bool any = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (trim(lines[i]).empty()) continue;
    any = true;
    const auto fields = split(trim(lines[i]), ',');
    const std::string what = "dose " + std::to_string(out.value.doses.size());
    Json notes = Json::object();
    const auto power = fields.size() == 3 ? number(fields[0]) : std::nullopt;
    const auto start = power ? to_utc(fields[1], lab_time_zone, what + " start", notes) : std::nullopt;
    const auto end = start ? to_utc(fields[2], lab_time_zone, what + " end", notes) : std::nullopt;
    if (!end) {
      out.detail["uninterpreted_lines"].push_back(line_note(i, lines[i]));
      continue;
    }
    for (auto& note : notes["notes"]) out.detail["notes"].push_back(std::move(note));
    out.value.doses.push_back({static_cast<int>(out.value.doses.size()), *power, *start, *end});
  }
  if (any && out.value.doses.empty()) return fail(ErrorKind::Protocol, "chronology has no line \"power,start,end\"");
  return out;
}

// ---------------------------------------------------------------- spectrometers

Result<ParsedGains> parse_gains(std::string_view json) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  if (!parsed->is_object()) return fail(ErrorKind::Protocol, "gains file is not a JSON object");
  ParsedGains out;
  for (auto it = parsed->begin(); it != parsed->end(); ++it) {
    if (const auto gain = as_double(*it))
      out.value.gains.push_back({it.key(), *gain});
    else
      out.detail["extra"][it.key()] = *it;
  }
  if (const Json tokens = nonfinite_under(nonfinite, ""); !tokens.is_null()) out.detail["nonfinite"] = tokens;
  return out;
}

Result<std::vector<MetaEntry>> parse_sensitivities(std::string_view json) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  if (!parsed->is_array()) return fail(ErrorKind::Protocol, "sensitivity file is not a JSON list");
  std::vector<MetaEntry> entries = entries_of(*parsed, nonfinite, "");
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const Json& entry = entries[i].entry;
    const auto sensitivity = entry.is_object() ? entry.find("sensitivity") : entry.end();
    if (!entry.is_object() || sensitivity == entry.end() || !as_double(*sensitivity))
      return fail(ErrorKind::Protocol, "sensitivity entry " + std::to_string(i) + " has no numeric \"sensitivity\"");
  }
  return entries;
}

ParsedSensitivity sensitivity_value(const MetaEntry& entry, std::string_view lab_time_zone) {
  ParsedSensitivity out;
  Json rest = entry.entry;
  out.value.sensitivity = take_double(rest, "sensitivity").value_or(0.0);
  if (const auto date = rest.find("create_date"); date != rest.end() && date->is_string()) {
    out.value.create_date = to_utc(date->get<std::string>(), lab_time_zone, "create_date", out.detail);
    if (out.value.create_date) rest.erase(date);
  }
  if (!entry.nonfinite.is_null()) rest["nonfinite"] = entry.nonfinite;
  out.value.extra_json = extra_text(rest);
  return out;
}

// ---------------------------------------------------------------- holders

Result<ParsedHolder> parse_holder(std::string_view text) {
  const auto lines = lines_of(text);
  if (lines.empty()) return fail(ErrorKind::Protocol, "holder file is empty");
  const auto header = split(lines[0], ',');
  const auto radius = header.size() >= 2 ? number(header[1]) : std::nullopt;
  if (!radius) return fail(ErrorKind::Protocol, "holder file does not start with \"<shape>,<radius>\"");

  ParsedHolder out;
  out.detail["header"] = std::string(lines[0]);
  if (const std::string_view first = trim(header[0]); !first.empty() && !number(first))
    out.value.shape = std::string(first);
  out.value.radius = radius;
  if (header.size() >= 3) out.value.has_hole_numbers = as_bool(Json(std::string(trim(header[2])))).value_or(false);

  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string_view line = trim(lines[i]);
    if (line.empty()) continue;
    if (line.front() == '#') {
      out.detail["comments"].push_back(line_note(i, lines[i]));
      continue;
    }
    auto fields = split(line, ',');
    ps::HolderHole hole;
    hole.ordinal = static_cast<int>(out.value.holes.size());
    hole.hole_id = std::to_string(i);  // lines after the header, counted from 1
    if (out.value.has_hole_numbers && !fields.empty()) {
      hole.hole_id = std::string(trim(fields.front()));
      fields.erase(fields.begin());
    }
    const auto x = fields.size() == 2 || fields.size() == 3 ? number(fields[0]) : std::nullopt;
    const auto y = x ? number(fields[1]) : std::nullopt;
    const auto own = y && fields.size() == 3 ? number(fields[2]) : std::nullopt;
    if (!y || (fields.size() == 3 && !own) || hole.hole_id.empty()) {
      out.detail["uninterpreted_lines"].push_back(line_note(i, lines[i]));
      continue;
    }
    hole.x = *x;
    hole.y = *y;
    hole.radius = own;
    out.value.holes.push_back(std::move(hole));
  }
  return out;
}

}  // namespace pychron::dvc
