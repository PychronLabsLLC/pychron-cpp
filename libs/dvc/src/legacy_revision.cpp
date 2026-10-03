// Raw data and the revision kinds: intercepts, baselines, blanks, IC factors,
// tags, cosmogenic (tests/dvc/fixtures/README.md, sections 5.2 to 5.6).

#include <cctype>
#include <initializer_list>
#include <utility>

#include "legacy_json.hpp"
#include "pychron/dvc/legacy_layout.hpp"
#include "pychron/persistence/blob.hpp"

namespace pychron::dvc {

namespace ps = pychron::persistence;

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string trimmed(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}

bool is_error_type(std::string_view lowered) { return lowered == "sd" || lowered == "sem"; }

// The curve fits the store reader knows (reduction::parse_fit_kind after
// lower-casing), from the spellings legacy pychron wrote: any case,
// "quadratic", anything with "average" in it, and the old combined
// "<fit>_<error type>". nullopt: not one of them; the caller keeps the text.
struct CurveFit {
  std::string fit;
  std::optional<std::string> error_type;  // from the combined form
};
std::optional<CurveFit> curve_fit(std::string_view legacy) {
  std::string name = lower(trimmed(legacy));
  CurveFit out;
  if (const auto underscore = name.find('_'); underscore != std::string::npos) {
    const std::string tail = name.substr(underscore + 1);
    if (!is_error_type(tail)) return std::nullopt;
    out.error_type = upper(tail);
    name.resize(underscore);
  }
  if (name == "quadratic") name = "parabolic";
  if (name.find("average") != std::string::npos) name = "average";
  for (const std::string_view known : {"average", "linear", "parabolic", "cubic", "exponential"})
    if (name == known) {
      out.fit = name;
      return out;
    }
  return std::nullopt;
}

// fit and error_type of an intercept or baseline entry, in the store's
// spelling; what was changed is remembered in `entry` (which becomes extra).
void take_curve_fit(Json& entry, std::optional<std::string>& fit, std::optional<std::string>& error_type) {
  const auto legacy_fit = take_text(entry, "fit");
  const auto legacy_error = take_text(entry, "error_type");
  std::optional<std::string> implied_error;
  if (legacy_fit) {
    if (auto curve = curve_fit(*legacy_fit)) {
      fit = curve->fit;
      implied_error = curve->error_type;
      if (*fit != *legacy_fit) entry["legacy_fit"] = *legacy_fit;
    } else {
      fit = legacy_fit;
    }
  }
  if (legacy_error) {
    error_type = is_error_type(lower(*legacy_error)) ? upper(*legacy_error) : *legacy_error;
    if (*error_type != *legacy_error) entry["legacy_error_type"] = *legacy_error;
  } else {
    error_type = implied_error;
  }
}

void take_manual(Json& entry, ps::ManualOverride& manual) {
  manual.use_value = take_bool(entry, "use_manual_value").value_or(false);
  manual.value = take_double(entry, "manual_value");
  manual.use_error = take_bool(entry, "use_manual_error").value_or(false);
  manual.error = take_double(entry, "manual_error");
}

// `exclude` was written as a boolean, as a status string ("ok", "omit", ...)
// and as a number.
bool excluded(const Json& j) {
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number()) return j.get<double>() != 0;
  if (j.is_string()) {
    const std::string s = lower(trimmed(j.get_ref<const std::string&>()));
    return !s.empty() && s != "ok" && s != "false";
  }
  return false;
}

// `references`: a list of {"record_id", "uuid", "exclude"}; "" or null for
// none. Whatever says more than the rows can is left in `entry` verbatim.
std::vector<ps::ReferenceRow> take_references(Json& entry) {
  std::vector<ps::ReferenceRow> rows;
  const auto it = entry.find("references");
  if (it == entry.end()) return rows;
  const Json& refs = *it;
  if (refs.is_null() || (refs.is_string() && refs.get_ref<const std::string&>().empty()) ||
      (refs.is_array() && refs.empty())) {
    entry.erase(it);
    return rows;
  }
  if (!refs.is_array()) return rows;  // an unexpected type: kept in extra
  int ordinal = 0;
  for (const auto& ref : refs) {
    ps::ReferenceRow row;
    row.ordinal = ordinal++;
    if (ref.is_object()) {
      if (const auto id = ref.find("record_id"); id != ref.end()) {
        if (auto text = as_text(*id); text && !text->empty()) row.record_id = *text;
      }
      if (const auto uuid = ref.find("uuid"); uuid != ref.end() && uuid->is_string())
        row.ref_analysis = ps::Uuid::parse(uuid->get_ref<const std::string&>());
      if (const auto exclude = ref.find("exclude"); exclude != ref.end()) row.exclude = excluded(*exclude);
    } else if (auto text = as_text(ref)) {
      row.record_id = *text;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

// The rest of an entry, plus the non-finite tokens that were under it.
std::optional<std::string> entry_extra(Json& entry, const std::vector<NonFinite>& nonfinite, std::string_view key) {
  if (Json tokens = nonfinite_under(nonfinite, pointer_of(key)); !tokens.is_null())
    entry["nonfinite"] = std::move(tokens);
  return extra_text(entry);
}

ps::InterceptRow intercept_row(const std::string& isotope, Json entry, const std::vector<NonFinite>& nonfinite) {
  ps::InterceptRow row;
  row.isotope = isotope;
  row.value = take_double(entry, "value");
  row.error = take_double(entry, "error");
  take_curve_fit(entry, row.fit, row.error_type);
  row.n = take_int(entry, "n");
  row.fn = take_int(entry, "fn");
  row.include_baseline_error = take_bool(entry, "include_baseline_error");
  row.filter_outliers_json = take_json(entry, "filter_outliers_dict");
  row.reviewed = take_bool(entry, "reviewed").value_or(false);
  take_manual(entry, row.manual);
  row.extra_json = entry_extra(entry, nonfinite, isotope);
  return row;
}

ps::BaselineRow baseline_row(const std::string& detector, Json entry, const std::vector<NonFinite>& nonfinite) {
  ps::BaselineRow row;
  row.detector = detector;
  row.value = take_double(entry, "value");
  row.error = take_double(entry, "error");
  take_curve_fit(entry, row.fit, row.error_type);
  row.n = take_int(entry, "n");
  row.fn = take_int(entry, "fn");
  row.filter_outliers_json = take_json(entry, "filter_outliers_dict");
  row.modifier_value = take_double(entry, "modifier_value");
  row.modifier_error = take_double(entry, "modifier_error");
  row.reviewed = take_bool(entry, "reviewed").value_or(false);
  take_manual(entry, row.manual);
  row.extra_json = entry_extra(entry, nonfinite, detector);
  return row;
}

ps::BlankRow blank_row(const std::string& isotope, Json entry, const std::vector<NonFinite>& nonfinite) {
  ps::BlankRow row;
  row.isotope = isotope;
  row.value = take_double(entry, "value");
  row.error = take_double(entry, "error");
  row.fit = take_text(entry, "fit");
  row.error_type = take_text(entry, "error_type");
  row.reviewed = take_bool(entry, "reviewed").value_or(false);
  take_manual(entry, row.manual);
  row.references = take_references(entry);
  row.extra_json = entry_extra(entry, nonfinite, isotope);
  return row;
}

ps::IcFactorRow icfactor_row(const std::string& detector, Json entry, const std::vector<NonFinite>& nonfinite) {
  ps::IcFactorRow row;
  row.detector = detector;
  row.value = take_double(entry, "value");
  row.error = take_double(entry, "error");
  row.fit = take_text(entry, "fit");
  row.reviewed = take_bool(entry, "reviewed").value_or(false);
  row.standard_ratio = take_double(entry, "standard_ratio");
  row.source_correction = take_bool(entry, "source_correction").value_or(false);
  take_manual(entry, row.manual);
  row.references = take_references(entry);
  row.extra_json = entry_extra(entry, nonfinite, detector);
  return row;
}

// One row per entry. A top-level value that is not an entry (not an object)
// has no row to go to: it comes back in the revision's extra, with its
// non-finite token if it was one. A file with such values and no entry at all
// is not a revision file.
template <class Row, class Make>
Result<ParsedRevision> keyed_rows(const Json& doc, const std::vector<NonFinite>& nonfinite, Make make) {
  std::vector<Row> rows;
  Json extra;
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    if (it.value().is_object()) {
      rows.push_back(make(it.key(), it.value(), nonfinite));
      continue;
    }
    extra[it.key()] = it.value();
    for (const auto& token : nonfinite)
      if (token.pointer == pointer_of(it.key())) extra["nonfinite"][token.pointer] = token.token;
  }
  if (rows.empty() && !extra.is_null())
    return fail(ErrorKind::Protocol, "entry \"" + extra.begin().key() + "\" is not an object");
  return ParsedRevision{std::move(rows), extra_text(extra)};
}

Result<ParsedRevision> tag_revision(Json doc, const std::vector<NonFinite>& nonfinite) {
  ps::TagValue tag;
  auto name = take_text(doc, "name");
  if (!name) return fail(ErrorKind::Protocol, "tags file has no \"name\"");
  tag.name = std::move(*name);
  tag.note = take_text(doc, "note");
  if (const auto it = doc.find("subgroup"); it != doc.end()) {
    if (!it->is_null() && !(it->is_string() && it->get_ref<const std::string&>().empty())) tag.subgroup_json = dump(*it);
    doc.erase(it);
  }
  if (Json tokens = nonfinite_under(nonfinite, ""); !tokens.is_null()) doc["nonfinite"] = std::move(tokens);
  return ParsedRevision{std::move(tag), extra_text(doc)};
}

// One list of {"isotope", "detector", "blob"} entries of a raw data file.
Result<void> take_series(Json& doc, std::string_view list, std::string_view series_kind, bool keyed_by_isotope,
                         ParsedData& out, Json& extra) {
  const auto it = doc.find(list);
  if (it == doc.end()) return {};
  if (!it->is_array()) return fail(ErrorKind::Protocol, "\"" + std::string(list) + "\" is not a list");
  Json list_extra;
  std::size_t index = 0;
  for (Json entry : *it) {
    const std::string where = std::string(list) + "[" + std::to_string(index) + "]";
    if (!entry.is_object()) return fail(ErrorKind::Protocol, where + " is not an object");
    ps::SignalRefRow ref;
    ref.series_kind = std::string(series_kind);
    ref.detector = take_text(entry, "detector").value_or("");
    ref.series_key = keyed_by_isotope ? take_text(entry, "isotope").value_or("") : ref.detector;
    if (ref.series_key.empty())
      return fail(ErrorKind::Protocol, where + " has no " + (keyed_by_isotope ? "isotope" : "detector"));
    const auto blob = entry.find("blob");
    if (blob == entry.end() || !blob->is_string()) return fail(ErrorKind::Protocol, where + " has no blob");
    auto points = ps::decode_legacy_ff_base64(blob->get_ref<const std::string&>());
    if (!points) return fail(ErrorKind::Protocol, where + ": " + points.error().what);
    entry.erase(blob);
    ps::BlobIngest ingest;
    ingest.codec = std::string(ps::kCodecTv);
    ingest.bytes = ps::encode_tv(*points);
    ingest.n_points = static_cast<int>(points->size());
    ref.blob_sha = ps::blob_sha256(ingest.codec, ingest.bytes);
    ref.n_points = ingest.n_points;
    out.blobs.push_back(std::move(ingest));
    out.refs.push_back(std::move(ref));
    if (!entry.empty()) list_extra[std::to_string(index)] = std::move(entry);
    ++index;
  }
  doc.erase(it);
  if (!list_extra.is_null()) extra[std::string(list)] = std::move(list_extra);
  return {};
}

}  // namespace

Result<ParsedData> parse_data(std::string_view json) {
  auto parsed = parse_legacy(json);
  if (!parsed) return fail(parsed.error());
  Json doc = std::move(*parsed);
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "raw data file is not a JSON object");
  // The blobs are read as base64 of big-endian float32 pairs; a file that
  // says otherwise cannot be.
  if (const auto format = take_text(doc, "format"); format && *format != ">ff")
    return fail(ErrorKind::Protocol, "unsupported raw data format \"" + *format + "\"");
  if (const auto encoding = take_text(doc, "encoding"); encoding && *encoding != "base64")
    return fail(ErrorKind::Protocol, "unsupported raw data encoding \"" + *encoding + "\"");
  if (doc.contains("format") || doc.contains("encoding"))
    return fail(ErrorKind::Protocol, "raw data format or encoding is not text");

  ParsedData out;
  Json extra;
  if (auto ok = take_series(doc, "signals", "signal", true, out, extra); !ok) return fail(ok.error());
  if (auto ok = take_series(doc, "baselines", "baseline", false, out, extra); !ok) return fail(ok.error());
  if (auto ok = take_series(doc, "sniffs", "sniff", true, out, extra); !ok) return fail(ok.error());
  for (auto it = doc.begin(); it != doc.end(); ++it) extra[it.key()] = it.value();
  out.extra_json = extra_text(extra);
  return out;
}

Result<ParsedRevision> parse_revision(FileKind kind, std::string_view json) {
  switch (kind) {
    case FileKind::Intercepts:
    case FileKind::Baselines:
    case FileKind::Blanks:
    case FileKind::IcFactors:
    case FileKind::Tags:
    case FileKind::Cosmogenic:
      break;
    default:
      return fail(ErrorKind::Config, "not a revision file kind");
  }
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  Json doc = std::move(*parsed);
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "revision file is not a JSON object");

  switch (kind) {
    case FileKind::Intercepts:
      return keyed_rows<ps::InterceptRow>(doc, nonfinite, intercept_row);
    case FileKind::Baselines:
      return keyed_rows<ps::BaselineRow>(doc, nonfinite, baseline_row);
    case FileKind::Blanks:
      return keyed_rows<ps::BlankRow>(doc, nonfinite, blank_row);
    case FileKind::IcFactors:
      return keyed_rows<ps::IcFactorRow>(doc, nonfinite, icfactor_row);
    case FileKind::Tags:
      return tag_revision(std::move(doc), nonfinite);
    default: {  // Cosmogenic: no real file has been seen, so the document is kept whole
      Json extra;
      if (Json tokens = nonfinite_under(nonfinite, ""); !tokens.is_null()) extra["nonfinite"] = std::move(tokens);
      return ParsedRevision{ps::CosmogenicValue{dump(doc)}, extra_text(extra)};
    }
  }
}

}  // namespace pychron::dvc
