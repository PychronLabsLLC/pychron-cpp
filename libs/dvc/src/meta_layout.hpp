#pragma once

// The layout of a legacy Python-pychron MetaData repository (legacy ingestion
// spec, section 4.3): which reference file a path names, and how each maps
// onto the reference payloads of the store. Pure: no git, no store. Private
// to pychron_dvc (it speaks Json). The authority on the format is
// tests/dvc/fixtures/README.md, section 6.
//
// The rules of legacy_layout.hpp hold here too. A key the payload type has
// no field for goes to the payload's extra; where the type has none, to the
// `detail` of the Parsed* result, which the caller keeps in the revision's
// provenance. A bare NaN reads as unknown and is recorded under "nonfinite".
// JSON of the wrong shape is an error. The two text formats (chronology,
// holders) keep each line they cannot interpret in `detail`, and are an error
// only when the file as a whole cannot be read.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "legacy_json.hpp"
#include "pychron/core/error.hpp"
#include "pychron/persistence/model.hpp"

namespace pychron::dvc {

// ---------------------------------------------------------------- paths

enum class MetaKind {
  Level,              // <irradiation>/<level>.json
  LevelProductions,   // <irradiation>/productions.json
  Production,         // <irradiation>/productions/<name>.json
  Chronology,         // <irradiation>/chronology.txt
  Gains,              // spectrometers/<name>.gain.json
  Sensitivity,        // spectrometers/<name>.sens.json
  IrradiationHolder,  // irradiation_holders/<name>.txt
  LoadHolder,         // load_holders/<name>.txt
  Ignored             // anything else: the repository holds much that is not reference data
};

// `irradiation`: set for Level, LevelProductions, Production, Chronology.
// `name`: the level, the production, the spectrometer as written, the holder.
struct MetaPath {
  MetaKind kind = MetaKind::Ignored;
  std::string irradiation, name;
};

// `repo_path`: relative to the repository root, '/' separated, as git prints
// it. An irradiation is any top-level directory but the ones the repository
// keeps for other things (spectrometers, irradiation_holders, load_holders,
// productions, scripts, experiments) and those whose name starts with '.'.
// A name is never empty and never starts with '.'.
MetaPath classify_meta_path(std::string_view repo_path);

// ---------------------------------------------------------------- level file

// One entry of a JSON list as the file has it, with the bare NaN and Infinity
// tokens that were in it ({"<pointer in the entry>": "<token>"}, or null).
// Two entries are the same when both parts are.
struct MetaEntry {
  Json entry;
  Json nonfinite;
  friend bool operator==(const MetaEntry&, const MetaEntry&) = default;
};

struct ParsedLevel {
  // The top-level keys other than "positions" ("z"); empty for the older form,
  // a bare list of positions.
  Json header = Json::object();
  Json header_nonfinite;
  // By hole number, in file order: the entries that carry it. The legacy code
  // reads the first; a level file normally has one.
  std::map<int, std::vector<MetaEntry>> positions;
};

// {"positions": [...], "z": ...} or a bare list. Every entry must be an
// object with an integral "position".
Result<ParsedLevel> parse_level(std::string_view json);

struct ParsedFlux {
  persistence::FluxValue value;
  Json detail = Json::object();  // "duplicate_entries": the entries after the first
};

// The flux of one position, from its entries (never empty). j, j_err, mean_j,
// mean_j_err, mean_j_mswd and position_jerr are taken as they are named;
// decay_constants.lambda_k_total and .lambda_k_total_error; monitor.name,
// .material, .age and .error; "options" verbatim. An entry of "analyses"
// becomes a FluxAnalysis when it is an object with a "record_id" not seen
// before: "uuid" when it is one, and the omitted flag from "is_omitted" or,
// in older files, "status". Everything else is in extra_json: "identifier"
// and any other key of the position, what is left of "monitor" and
// "decay_constants", the other keys of an analysis under
// "analyses"."<record_id>", an analysis that could not be mapped under
// "analyses_unmapped", and "nonfinite".
ParsedFlux flux_value(const std::vector<MetaEntry>& entries);

struct ParsedLevelZ {
  persistence::LevelZValue value;
  Json detail = Json::object();  // "extra": header keys other than a numeric "z"; "nonfinite"
};
ParsedLevelZ level_z_value(const ParsedLevel& level);

// ---------------------------------------------------------------- productions.json

struct ParsedLevelProductions {
  std::map<std::string, std::string> levels;  // level name -> production name
  std::optional<std::string> note;            // the file's "note"
  Json extra = Json::object();                // values that are not a production name
};

// {"<level>": "<production name>", ..., "note": "..."}.
Result<ParsedLevelProductions> parse_level_productions(std::string_view json);

// ---------------------------------------------------------------- chronology

struct ParsedChronology {
  persistence::ChronologyValue value;  // ordinals count the doses from 0
  // "uninterpreted_lines": [{"line": n (from 1), "text": "..."}]; "notes": a
  // local time that is ambiguous or does not exist in the zone.
  Json detail = Json::object();
};

// One dose per line, "power,start,end", the times naive local
// "YYYY-MM-DD HH:MM:SS" in `lab_time_zone`. Blank lines are skipped. An error
// when there are lines and none is a dose, or the zone is unknown.
Result<ParsedChronology> parse_chronology(std::string_view text, std::string_view lab_time_zone);

// ---------------------------------------------------------------- spectrometers

struct ParsedGains {
  persistence::GainsValue value;
  Json detail = Json::object();  // "extra": values that are not numbers; "nonfinite"
};

// {"<detector>": gain, ...}; `{}` is a value without gains.
Result<ParsedGains> parse_gains(std::string_view json);

// The sensitivity list, in file order. Every entry must be an object with a
// numeric "sensitivity".
Result<std::vector<MetaEntry>> parse_sensitivities(std::string_view json);

struct ParsedSensitivity {
  persistence::SensitivityValue value;
  Json detail = Json::object();  // "notes": as ParsedChronology
};

// "create_date" is naive local time; one that cannot be read stays in
// extra_json as written, with every other key ("mass_spectrometer", "units",
// "note", "user").
ParsedSensitivity sensitivity_value(const MetaEntry& entry, std::string_view lab_time_zone);

// ---------------------------------------------------------------- holders

struct ParsedHolder {
  persistence::HolderValue value;  // ordinals count the holes from 0
  // "header": the first line as written; "comments" and "uninterpreted_lines"
  // as ParsedChronology's lines.
  Json detail = Json::object();
};

// Irradiation and load holders share one format. The first line is
// "<shape or count>,<radius>[,<has hole numbers>]": the first field is the
// shape when it is not a number. Then one hole per line, "x,y" or "x,y,r", or
// "id,x,y[,r]" when the header says the holes are numbered. A hole without an
// id is numbered by its line, counted from 1 after the header; blank lines
// and comments ('#') count, as in the legacy reader. An error when there is
// no header or its radius is not a number.
Result<ParsedHolder> parse_holder(std::string_view text);

}  // namespace pychron::dvc
