// The analysis record and the files that fold into it: extraction, peak
// center, monitor, spectrometer settings (tests/dvc/fixtures/README.md,
// sections 5.1, 5.7, 5.8).

#include <initializer_list>
#include <iterator>
#include <utility>

#include "legacy_json.hpp"
#include "pychron/dvc/legacy_layout.hpp"
#include "pychron/ingest/tz.hpp"
#include "pychron/persistence/blob.hpp"

namespace pychron::dvc {

namespace ps = pychron::persistence;

namespace {

// A copy of `key` as text, leaving it in the object ("" and null: nullopt).
std::optional<std::string> peek_text(const Json& object, std::string_view key) {
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  auto text = as_text(*it);
  if (text && text->empty()) return std::nullopt;
  return text;
}

Result<ps::UtcTime> to_utc(const std::string& naive, std::string_view what, const ParseContext& ctx,
                           std::vector<std::string>& notes) {
  auto converted = ingest::local_to_utc(naive, ctx.lab_time_zone);
  if (!converted) return fail(ErrorKind::Protocol, std::string(what) + " \"" + naive + "\": " + converted.error().what);
  if (converted->kind == ingest::LocalKind::Ambiguous)
    notes.push_back(std::string(what) + " " + naive + " is ambiguous in " + ctx.lab_time_zone +
                    " (clocks set back): the earlier instant was taken");
  if (converted->kind == ingest::LocalKind::Nonexistent)
    notes.push_back(std::string(what) + " " + naive + " is nonexistent in " + ctx.lab_time_zone +
                    " (clocks set forward): the start of the gap was taken");
  return converted->utc;
}

ps::AnalysisMetaRow& meta_of(ps::AnalysisIngest& a) {
  if (!a.meta) a.meta.emplace();
  return *a.meta;
}

// meta.legacy_json is one object with a member per legacy file ("record",
// "extraction", ...) holding that file's unmapped keys, and "nonfinite" with
// the same members for the NaN / Infinity tokens of each file.
Result<void> keep_leftover(ps::AnalysisIngest& a, const std::string& section, const Json& rest,
                           const std::vector<NonFinite>& nonfinite) {
  const Json tokens = nonfinite_under(nonfinite, "");
  const bool has_rest = !rest.is_null() && !(rest.is_object() && rest.empty());
  if (!has_rest && tokens.is_null()) return {};
  auto& meta = meta_of(a);
  Json legacy = Json::object();
  if (meta.legacy_json) {
    auto existing = parse_legacy(*meta.legacy_json);
    if (!existing || !existing->is_object()) return fail(ErrorKind::Protocol, "meta.legacy_json is not a JSON object");
    legacy = std::move(*existing);
  }
  if (has_rest) legacy[section] = rest;
  if (!tokens.is_null()) legacy["nonfinite"][section] = tokens;
  meta.legacy_json = dump(legacy);
  return {};
}

// Removes `key` from `parent` when the object under it has become empty.
void drop_if_empty(Json& parent, std::string_view key) {
  const auto it = parent.find(key);
  if (it != parent.end() && it->is_object() && it->empty()) parent.erase(it);
}

// {"<name>": {...}} -> rows; each entry keeps only what its row cannot hold.
template <class Row, class Fill>
void take_named(Json& doc, std::string_view key, std::vector<Row>& rows, Fill fill) {
  const auto it = doc.find(key);
  if (it == doc.end() || !it->is_object()) return;
  for (auto entry = it->begin(); entry != it->end();) {
    if (!entry.value().is_object()) {
      ++entry;
      continue;
    }
    rows.push_back(fill(entry.key(), entry.value()));
    entry = entry.value().empty() ? it->erase(entry) : std::next(entry);
  }
  drop_if_empty(doc, key);
}

// Moves the listed keys of `doc` into one JSON object.
std::optional<std::string> take_group(Json& doc, std::initializer_list<std::string_view> keys) {
  Json group;
  for (const auto key : keys) {
    const auto it = doc.find(key);
    if (it == doc.end()) continue;
    group[std::string(key)] = *it;
    doc.erase(it);
  }
  return extra_text(group);
}

Result<ps::BlobIngest> ff_blob(const std::string& base64, const std::string& where) {
  auto points = ps::decode_legacy_ff_base64(base64);
  if (!points) return fail(ErrorKind::Protocol, where + ": " + points.error().what);
  ps::BlobIngest blob;
  blob.codec = std::string(ps::kCodecTv);
  blob.bytes = ps::encode_tv(*points);
  blob.n_points = static_cast<int>(points->size());
  return blob;
}

Result<void> merge_extraction(Json doc, const std::vector<NonFinite>& nonfinite, ps::AnalysisIngest& a) {
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "extraction file is not a JSON object");
  auto& x = a.extraction;
  auto& meta = meta_of(a);
  if (auto v = take_text(doc, "extract_device")) a.extract_device = std::move(v);
  x.extract_value = take_double(doc, "extract_value");
  x.extract_units = take_text(doc, "extract_units");
  // Older files say `duration` and `cleanup`; the current names win.
  x.extract_duration = take_double(doc, "extract_duration");
  if (!x.extract_duration && !doc.contains("extract_duration")) x.extract_duration = take_double(doc, "duration");
  x.cleanup_duration = take_double(doc, "cleanup_duration");
  if (!x.cleanup_duration && !doc.contains("cleanup_duration")) x.cleanup_duration = take_double(doc, "cleanup");
  x.pre_cleanup = take_double(doc, "pre_cleanup_duration");
  x.post_cleanup = take_double(doc, "post_cleanup_duration");
  x.cryo_temperature = take_double(doc, "cryo_temperature");
  x.weight = take_double(doc, "weight");
  x.beam_diameter = take_double(doc, "beam_diameter");
  x.pattern = take_text(doc, "pattern");
  x.ramp_duration = take_double(doc, "ramp_duration");
  x.ramp_rate = take_double(doc, "ramp_rate");
  x.light_value = take_double(doc, "light_value");
  x.tray = take_text(doc, "tray");
  if (auto v = take_text(doc, "load_name")) a.load_name = std::move(v);
  if (auto v = take_text(doc, "load_holder")) a.load_holder = std::move(v);
  meta.extraction_context_json = take_json(doc, "extraction_context");
  meta.snapshots_json = take_json(doc, "snapshots");
  meta.videos_json = take_json(doc, "videos");
  meta.grain_polygons_json = take_json(doc, "grain_polygons");
  if (!meta.grain_polygons_json) meta.grain_polygons_json = take_json(doc, "grain_polygon_blob");

  if (const auto it = doc.find("positions"); it != doc.end() && it->is_array()) {
    bool whole = true;  // every entry is held in full by its row
    for (Json entry : *it) {
      ps::MeasuredPositionRow row;
      row.load_name = a.load_name;
      if (entry.is_object()) {
        row.position = take_int(entry, "position");
        row.x = take_double(entry, "x");
        row.y = take_double(entry, "y");
        row.z = take_double(entry, "z");
        row.is_degas = take_bool(entry, "is_degas").value_or(false);
        if (!entry.empty()) whole = false;
      } else {
        row.position = as_int(entry);
        if (!row.position) whole = false;
      }
      a.measured_positions.push_back(std::move(row));
    }
    if (whole) doc.erase(it);
  }
  return keep_leftover(a, "extraction", doc, nonfinite);
}

Result<void> merge_peak_center(Json doc, const std::vector<NonFinite>& nonfinite, ps::AnalysisIngest& a,
                               std::vector<ps::BlobIngest>& blobs_out) {
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "peak center file is not a JSON object");
  const auto fmt = take_text(doc, "fmt");
  const auto reference_detector = take_text(doc, "reference_detector");
  const auto reference_isotope = take_text(doc, "reference_isotope");
  const auto interpolation = take_text(doc, "interpolation");

  std::vector<ps::PeakCenterRow> rows;
  std::vector<ps::BlobIngest> blobs;
  for (auto it = doc.begin(); it != doc.end();) {
    // Every object is a detector, except the older top-level `data` form,
    // which is kept and not read.
    if (!it.value().is_object() || it.key() == "data") {
      ++it;
      continue;
    }
    Json& entry = it.value();
    ps::PeakCenterRow row;
    row.detector = it.key();
    row.reference_detector = reference_detector;
    row.reference_isotope = reference_isotope;
    row.interpolation = take_text(entry, "interpolation");
    if (!row.interpolation) row.interpolation = interpolation;
    row.low_dac = take_double(entry, "low_dac");
    row.center_dac = take_double(entry, "center_dac");
    row.high_dac = take_double(entry, "high_dac");
    row.low_signal = take_double(entry, "low_signal");
    row.center_signal = take_double(entry, "center_signal");
    row.high_signal = take_double(entry, "high_signal");
    row.resolution = take_double(entry, "resolution");
    row.low_resolving_power = take_double(entry, "low_resolving_power");
    row.high_resolving_power = take_double(entry, "high_resolving_power");
    if (const auto points = entry.find("points"); points != entry.end()) {
      if (points->is_string() && !points->get_ref<const std::string&>().empty()) {
        if (fmt && *fmt != ">ff")
          return fail(ErrorKind::Protocol, "unsupported peak center format \"" + *fmt + "\"");
        auto blob = ff_blob(points->get_ref<const std::string&>(), "peak center " + row.detector);
        if (!blob) return fail(blob.error());
        row.points_blob_sha = ps::blob_sha256(blob->codec, blob->bytes);
        blobs.push_back(std::move(*blob));
        entry.erase(points);
      } else if (points->is_null() || points->is_string()) {
        entry.erase(points);  // no scan
      }
    }
    rows.push_back(std::move(row));
    it = entry.empty() ? doc.erase(it) : std::next(it);
  }
  if (fmt && *fmt != ">ff") doc["fmt"] = *fmt;  // no scan used it; keep what it said
  a.peak_centers.insert(a.peak_centers.end(), std::make_move_iterator(rows.begin()),
                        std::make_move_iterator(rows.end()));
  blobs_out.insert(blobs_out.end(), std::make_move_iterator(blobs.begin()), std::make_move_iterator(blobs.end()));
  return keep_leftover(a, "peakcenter", doc, nonfinite);
}

Result<void> merge_monitor(Json doc, const std::vector<NonFinite>& nonfinite, ps::AnalysisIngest& a,
                           std::vector<ps::BlobIngest>& blobs_out) {
  if (!doc.is_array()) return fail(ErrorKind::Protocol, "monitor file is not a JSON list");
  std::vector<ps::MonitorCheckRow> rows;
  std::vector<ps::BlobIngest> blobs;
  Json rest;
  int ordinal = 0;
  for (Json entry : doc) {
    const std::string where = "monitor check " + std::to_string(ordinal);
    if (!entry.is_object()) return fail(ErrorKind::Protocol, where + " is not an object");
    ps::MonitorCheckRow row;
    row.ordinal = ordinal;
    row.name = take_text(entry, "name");
    row.parameter = take_text(entry, "parameter");
    row.criterion = take_text(entry, "criterion");
    row.comparator = take_text(entry, "comparator");
    row.tripped = take_bool(entry, "tripped");
    if (const auto data = entry.find("data"); data != entry.end() && data->is_string()) {
      if (!data->get_ref<const std::string&>().empty()) {
        auto blob = ff_blob(data->get_ref<const std::string&>(), where);
        if (!blob) return fail(blob.error());
        row.data_blob_sha = ps::blob_sha256(blob->codec, blob->bytes);
        blobs.push_back(std::move(*blob));
      }
      entry.erase(data);
    }
    if (!entry.empty()) rest[std::to_string(ordinal)] = std::move(entry);
    rows.push_back(std::move(row));
    ++ordinal;
  }
  a.monitor_checks.insert(a.monitor_checks.end(), std::make_move_iterator(rows.begin()),
                          std::make_move_iterator(rows.end()));
  blobs_out.insert(blobs_out.end(), std::make_move_iterator(blobs.begin()), std::make_move_iterator(blobs.end()));
  return keep_leftover(a, "monitor", rest, nonfinite);
}

}  // namespace

Result<ParsedRecord> parse_record(std::string_view json, const ParseContext& ctx) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  Json doc = std::move(*parsed);
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "analysis record is not a JSON object");

  ParsedRecord out;
  auto& a = out.ingest;

  // Identity. A key leaves `doc` only when a field of the ingest carries it;
  // what is left is kept in meta.legacy_json.
  if (const auto it = doc.find("uuid"); it != doc.end()) {
    const auto text = as_text(*it);
    std::optional<ps::Uuid> uuid;
    if (text) uuid = ps::Uuid::parse(*text);
    if (uuid && !uuid->is_nil()) {
      a.analysis = *uuid;
      doc.erase(it);
    } else if (it->is_null() || (text && text->empty())) {
      doc.erase(it);
    } else {
      out.notes.push_back("uuid " + dump(*it) + " is not a uuid; treated as missing");
    }
  }
  out.had_uuid = !a.analysis.is_nil();

  auto identifier = take_text(doc, "identifier");
  if (!identifier) return fail(ErrorKind::Protocol, "analysis record has no identifier");
  a.identifier = std::move(*identifier);
  const auto aliquot = take_int(doc, "aliquot");
  if (!aliquot) return fail(ErrorKind::Protocol, "analysis record has no integer aliquot");
  a.aliquot = *aliquot;
  a.increment = take_int(doc, "increment").value_or(-1);  // null: no step
  out.runid = make_runid(a.identifier, a.aliquot, a.increment);

  // The legacy reader maps "sample" and empty to "unknown"; the original
  // stays in legacy_json when it said something else.
  auto analysis_type = take_text(doc, "analysis_type");
  if (analysis_type && *analysis_type == "sample") doc["analysis_type"] = *analysis_type;
  a.analysis_type = !analysis_type || *analysis_type == "sample" ? "unknown" : std::move(*analysis_type);
  if (auto v = take_text(doc, "experiment_type")) a.experiment_type = std::move(v);

  const auto timestamp = take_text(doc, "timestamp");
  if (!timestamp) return fail(ErrorKind::Protocol, "analysis record has no timestamp");
  auto utc = to_utc(*timestamp, "timestamp", ctx, out.notes);
  if (!utc) return fail(utc.error());
  a.timestamp = *utc;
  if (const auto time_zero = take_text(doc, "time_zero_timestamp")) {
    auto zero = to_utc(*time_zero, "time_zero_timestamp", ctx, out.notes);
    if (!zero) return fail(zero.error());
    a.time_zero = *zero;
  }

  auto mass_spectrometer = take_text(doc, "mass_spectrometer");
  if (!mass_spectrometer) return fail(ErrorKind::Protocol, "analysis record has no mass_spectrometer");
  a.mass_spectrometer = std::move(*mass_spectrometer);
  a.laboratory = take_text(doc, "laboratory");
  a.instrument_name = take_text(doc, "instrument_name");
  if (auto username = take_text(doc, "username")) {
    a.analyst = std::move(*username);
  } else if (auto analyst_name = take_text(doc, "analyst_name")) {
    a.analyst = std::move(*analyst_name);
  }

  take_named(doc, "isotopes", a.isotopes, [](const std::string& isotope, Json& entry) {
    ps::IsotopeRow row;
    row.isotope = isotope;
    row.detector = take_text(entry, "detector").value_or("");
    row.units = take_text(entry, "units");
    row.detector_serial = take_text(entry, "serial_id");
    return row;
  });
  take_named(doc, "detectors", a.detectors, [](const std::string& detector, Json& entry) {
    ps::DetectorRow row;
    row.detector = detector;
    row.deflection = take_double(entry, "deflection");
    row.gain_used = take_double(entry, "gain");
    return row;
  });

  auto& meta = meta_of(a);
  meta.source_json = take_json(doc, "source");
  meta.environmental_json = take_json(doc, "environmental");
  meta.conditionals_json = take_json(doc, "conditionals");
  meta.tripped_conditional_json = take_json(doc, "tripped_conditional");
  meta.whiff_result_json = take_json(doc, "whiff_result");
  meta.intensity_scalar = take_double(doc, "intensity_scalar");
  meta.arar_mapping_json = take_json(doc, "arar_mapping");
  meta.software_json = take_group(doc, {"acquisition_software", "data_reduction_software"});
  meta.queue_names_json = take_group(doc, {"experiment_queue_name", "queue_conditionals_name"});

  // Carried beside the ingest, for the caller; not removed from `doc`.
  out.spec_sha = peek_text(doc, "spec_sha");
  out.comment = peek_text(doc, "comment");
  out.catalog.repository = peek_text(doc, "repository_identifier");
  out.catalog.sample = peek_text(doc, "sample");
  out.catalog.material = peek_text(doc, "material");
  out.catalog.project = peek_text(doc, "project");
  out.catalog.principal_investigator = peek_text(doc, "principal_investigator");
  out.catalog.irradiation = peek_text(doc, "irradiation");
  out.catalog.irradiation_level = peek_text(doc, "irradiation_level");
  if (const auto it = doc.find("irradiation_position"); it != doc.end()) out.catalog.irradiation_position = as_int(*it);
  out.script_names.measurement = peek_text(doc, "measurement");
  out.script_names.extraction = peek_text(doc, "extraction");
  out.script_names.post_measurement = peek_text(doc, "post_measurement");
  out.script_names.post_equilibration = peek_text(doc, "post_equilibration");

  if (auto kept = keep_leftover(a, "record", doc, nonfinite); !kept) return fail(kept.error());
  return out;
}

Result<void> merge_satellite(FileKind kind, std::string_view json, persistence::AnalysisIngest& into,
                             std::vector<persistence::BlobIngest>& blobs_out) {
  if (kind != FileKind::Extraction && kind != FileKind::PeakCenter && kind != FileKind::Monitor)
    return fail(ErrorKind::Config, "not a satellite file kind");
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  if (kind == FileKind::Extraction) return merge_extraction(std::move(*parsed), nonfinite, into);
  if (kind == FileKind::PeakCenter) return merge_peak_center(std::move(*parsed), nonfinite, into, blobs_out);
  return merge_monitor(std::move(*parsed), nonfinite, into, blobs_out);
}

Result<persistence::SpectrometerSnapshot> parse_spectrometer(std::string_view json, std::string_view legacy_sha1) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  Json doc = std::move(*parsed);
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "spectrometer file is not a JSON object");

  ps::SpectrometerSnapshot snapshot;
  if (!legacy_sha1.empty()) snapshot.legacy_sha1 = std::string(legacy_sha1);
  if (auto v = take_json(doc, "spectrometer")) snapshot.spectrometer_json = std::move(*v);
  if (auto v = take_json(doc, "gains")) snapshot.gains_json = std::move(*v);
  if (auto v = take_json(doc, "deflections")) snapshot.deflections_json = std::move(*v);
  Json settings = Json::object();
  if (const auto it = doc.find("settings"); it != doc.end() && it->is_object()) {
    settings = *it;
    doc.erase(it);
  }
  // The snapshot has no extra: what is left rides inside settings.
  if (Json tokens = nonfinite_under(nonfinite, ""); !tokens.is_null()) doc["nonfinite"] = std::move(tokens);
  if (!doc.empty()) settings["legacy_extra"] = std::move(doc);
  snapshot.settings_json = dump(settings);
  return snapshot;
}

}  // namespace pychron::dvc
