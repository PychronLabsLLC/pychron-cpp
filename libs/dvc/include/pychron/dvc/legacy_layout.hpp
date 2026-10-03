#pragma once

// The layout of a legacy Python-pychron DVC repository (legacy ingestion spec,
// sections 4.4 and 10): which kind of file a repository path names, and how
// each kind of legacy JSON maps onto the payload types of the new store.
//
// Everything here is pure: no git, no store. The authority on the format is
// tests/dvc/fixtures/README.md.
//
// Rules common to every parser:
//  - A key the target type has no field for is never dropped. It goes into the
//    row's `extra_json`, or, for a target without one, into the `extra_json`
//    of the Parsed* result, which the caller keeps in provenance.
//  - Python writes NaN, Infinity and -Infinity as bare tokens. A numeric field
//    holding one becomes nullopt (unknown), never 0, and the token is recorded
//    under "nonfinite" in the same extra, keyed by JSON pointer. In JSON that
//    is passed through as text (extra, doc_json, ...) the token becomes null.
//  - An empty string in a text field that names something (units, pattern,
//    laboratory, tag note, ...) is "not set": nullopt.
//  - Text that is not JSON, or JSON of the wrong shape, is an error.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/model.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::dvc {

// ---------------------------------------------------------------- paths

enum class FileKind {
  Record,            // <p>/<t>.json
  Data,              // <p>/.data/<t>.dat.json
  Intercepts,        // <p>/intercepts/<t>.inte.json
  Baselines,         // <p>/baselines/<t>.base.json
  Blanks,            // <p>/blanks/<t>.blan.json
  IcFactors,         // <p>/icfactors/<t>.icfa.json
  Tags,              // [reduction/]<p>/tags/<t>.tags.json
  PeakCenter,        // <p>/peakcenter/<t>.peak.json
  Extraction,        // <p>/extraction/<t>.extr.json
  Monitor,           // <p>/monitor/<t>.moni.json
  Cosmogenic,        // <p>/cosmogenic/<t>.cosm.json
  InterpretedAge,    // [reduction/]<p>/ia/<t>.ia.json
  Spectrometer,      // <40 hex>.json at the root
  FrozenProduction,  // <irradiation>.<level>.production.json at the root
  Ignored,           // README* and dotfiles (.gitignore) at the root; run logs <p>/logs/<t>.logs.log
  Unknown            // matches no known pattern
};

// `key` by kind:
//   Record .. Cosmogenic   <p><t>: the analysis runid (':' written as '_'), or
//                          its uuid in newer repositories (key_is_uuid). The
//                          prefix length varies, so the two are only joined.
//   InterpretedAge         <p><t>: "<identifier>" or "<identifier>_<NNNNN>"
//   Spectrometer           the 40 hex digits, as written
//   FrozenProduction       "<irradiation>.<level>" (split_frozen_production_key)
//   Ignored, Unknown       empty
struct PathInfo {
  FileKind kind = FileKind::Unknown;
  std::string key;
  bool key_is_uuid = false;
};

// `repo_path`: relative to the repository root, '/' separated, as git prints it.
PathInfo classify_path(std::string_view repo_path);

struct FrozenProductionKey {
  std::string irradiation, level;
};
// Splits at the last '.': "NM-312.F" -> {"NM-312", "F"}.
FrozenProductionKey split_frozen_production_key(std::string_view key);

// "<identifier>-<aliquot, two digits at least><step letters>"; increment 0 is
// "A", 25 "Z", 26 "AA"; a negative increment has no step. The store's rule
// (persistence::make_runid), under the name the legacy source uses.
std::string make_runid(std::string_view identifier, int aliquot, int increment);

// ---------------------------------------------------------------- analysis record

struct ParseContext {
  std::string lab_time_zone;  // IANA name; legacy timestamps are naive local time
};

// The catalog names a record carries: a copy made at collection (and rewritten
// by <SYNC> commits). They stay in meta.legacy_json as well.
struct RecordCatalog {
  std::optional<std::string> repository, sample, material, project, principal_investigator, irradiation,
      irradiation_level;
  std::optional<int> irradiation_position;
};

// Script file names (not bodies). They stay in meta.legacy_json as well.
struct RecordScriptNames {
  std::optional<std::string> measurement, extraction, post_measurement, post_equilibration;
};

struct ParsedRecord {
  // Set: analysis (nil when the file has no uuid), identity, analysis_type,
  // timestamp (UTC), time_zero, mass_spectrometer (lower case, as the catalog
  // and the meta repository name spectrometers; a different spelling in the
  // file stays in meta.legacy_json), experiment_type,
  // laboratory, instrument_name, analyst, isotopes, detectors, meta.
  // Not set: roots (parse_data, parse_revision), extraction and positions
  // (merge_satellite), spectrometer_snapshot (parse_spectrometer of the file
  // `spec_sha` names), everything the writer derives.
  persistence::AnalysisIngest ingest;
  bool had_uuid = true;
  std::string runid;                    // make_runid of the identity in the file
  std::optional<std::string> spec_sha;  // names <spec_sha>.json at the repository root
  std::optional<std::string> comment;   // also kept in meta.legacy_json
  RecordCatalog catalog;
  RecordScriptNames script_names;
  std::vector<std::string> notes;  // e.g. "timestamp ... is ambiguous in ..."
};

// <p>/<t>.json. identifier, aliquot, timestamp and mass_spectrometer are
// required. Keys with no typed home go to meta.legacy_json under "record".
Result<ParsedRecord> parse_record(std::string_view json, const ParseContext& ctx);

// Satellite files fold into the analysis: Extraction (extraction fields,
// extract device, load, measured positions, meta JSON columns), PeakCenter
// (peak_centers; the scan of each detector becomes a blob appended to
// `blobs_out`, codec f32le-tv/1 with t = DAC and v = signal), Monitor
// (monitor_checks; data blobs likewise). Unmapped keys go to
// meta.legacy_json under "extraction", "peakcenter" or "monitor". Any other
// kind is an error.
Result<void> merge_satellite(FileKind kind, std::string_view json, persistence::AnalysisIngest& into,
                             std::vector<persistence::BlobIngest>& blobs_out);

// <spec_sha>.json. `legacy_sha1` is the file stem (PathInfo::key). Unknown
// top-level keys are kept inside settings_json under "legacy_extra".
Result<persistence::SpectrometerSnapshot> parse_spectrometer(std::string_view json, std::string_view legacy_sha1);

// ---------------------------------------------------------------- raw data

// <p>/.data/<t>.dat.json. blobs[i] is the series refs[i] points at
// (refs[i].blob_sha == blob_sha256(blobs[i].codec, blobs[i].bytes)); every
// blob is f32le-tv/1. series_kind is "signal", "sniff" (series_key: the
// isotope) or "baseline" (series_key: the detector).
struct ParsedData {
  std::vector<persistence::BlobIngest> blobs;
  persistence::SignalRefs refs;
  std::optional<std::string> extra_json;  // keys with no home in SignalRefRow, e.g. "commit"
};
Result<ParsedData> parse_data(std::string_view json);

// ---------------------------------------------------------------- revisions

// Intercepts, Baselines, Blanks, IcFactors, Tags, Cosmogenic. Any other kind
// is an error.
//
// fit and error_type of intercepts and baselines are written in the store's
// spelling ("parabolic", "SEM"); when that differs from the file, the file's
// string is kept in extra_json as "legacy_fit" / "legacy_error_type". Blank
// and IC-factor fits ("previous", "Bracketing Interpolate", ...) name a
// method, not a curve, and are kept as written.
//
// References (blanks, IC factors) name other analyses, possibly in other
// repositories. Each becomes a ReferenceRow with `record_id` and, when the
// file gives a uuid, `ref_analysis`, both as written; nothing is looked up.
// `ref_analysis` is a foreign key in the store, so before writing the caller
// must clear it on every reference whose analysis is not there. Nothing is
// lost by that: a non-empty `references` value is also kept verbatim in
// extra_json under "references". `exclude` is true for boolean true, a
// non-zero number, or a string other than "" and "ok".
//
// TagValue has no extra field, so the unknown keys of a tags file come back in
// ParsedRevision::extra_json. So does a top-level value of an intercepts,
// baselines, blanks or IC-factor file that is not an entry (not an object),
// beside at least one that is. Otherwise it is unset: rows carry their own
// extra, and cosmogenic keeps the whole file as its document.
struct ParsedRevision {
  persistence::RevisionPayload payload;
  std::optional<std::string> extra_json;
};
Result<ParsedRevision> parse_revision(FileKind kind, std::string_view json);

// ---------------------------------------------------------------- interpreted ages

struct ParsedInterpretedAge {
  std::string name;
  std::optional<persistence::Uuid> uuid;  // of the interpreted age itself
  std::string identifier;                 // "identifier" key (2018 flat format), else from `path_key`
  bool nested = false;                    // the later format, with "preferred" and "sample_metadata"
  // age, age_err, age_kind: the "age" entry of preferred_kinds when it has a
  // value, else the top-level (flat) or preferred (nested) age and age_err.
  // kca and kca_err likewise. doc_json is the whole file. value.members has
  // the analyses whose uuid parses, in file order.
  persistence::InterpretedAgeValue value;
  // One per entry of "analyses", in file order: the legacy per-analysis ages
  // verify compares against. nullopt: absent, null or not finite.
  struct Member {
    std::optional<persistence::Uuid> analysis;
    std::string record_id;
    std::optional<double> age, age_err;
  };
  std::vector<Member> members;
  std::vector<std::string> notes;
};

// `path_key`: PathInfo::key of the file, used for the identifier when the
// file has none (the nested format).
Result<ParsedInterpretedAge> parse_interpreted_age(std::string_view json, std::string_view path_key = {});

// ---------------------------------------------------------------- frozen production

struct ParsedProduction {
  std::string irradiation, level;
  persistence::ProductionValue value;
  std::optional<std::string> name;        // the production's name, when the file has one
  std::optional<std::string> extra_json;  // keys that are not ratios, reactor, note or name
};

// <irradiation>.<level>.production.json: {"<ratio>": [value, error], ...}. A
// ratio that is not a finite number is an error.
Result<ParsedProduction> parse_frozen_production(std::string_view json, std::string_view irradiation,
                                                 std::string_view level);

}  // namespace pychron::dvc
