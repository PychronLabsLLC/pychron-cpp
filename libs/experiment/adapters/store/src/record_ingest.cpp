#include "pychron/experiment/store/record_ingest.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "pychron/core/sha256.hpp"
#include "pychron/experiment/record/names.hpp"
#include "pychron/persistence/blob.hpp"
#include "pychron/reduction/fits.hpp"

namespace pychron::experiment::store {

namespace ps = pychron::persistence;
using nlohmann::json;

namespace {

// A JSON value as text. Strings that are not UTF-8 (a name typed on another
// system, a device's reply) have the bad bytes replaced with U+FFFD rather
// than thrown at the caller: dump() with its default handler throws, and a
// record must not be unsavable, or end the program, for a stray byte.
std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::optional<double> finite(double v) {
  if (!std::isfinite(v)) return std::nullopt;
  return v;
}

std::optional<Sha256Digest> digest_of(const std::string& hex) {
  if (hex.size() != 64) return std::nullopt;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Sha256Digest out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return out;
}

// ---- identity: ids, times, the identity and instrument columns

Result<void> fill_identity(const record::AnalysisRecord& rec, const ps::Uuid& analysis, ps::AnalysisIngest& a) {
  const auto& id = rec.identity;
  const auto created = ps::UtcTime::parse(id.timestamp);
  if (!created) return fail(ErrorKind::Config, "the record's timestamp does not parse: " + id.timestamp);
  const auto step = increment_of(id.step);
  if (!step) return fail(ErrorKind::Config, "the record's step is not a letter sequence: " + id.step);

  a.analysis = analysis;
  a.changeset = ps::Uuid::v5(analysis, "changeset");
  a.created = *created;
  a.timestamp = *created;
  a.identifier = id.identifier;
  a.aliquot = id.aliquot;
  a.increment = *step;
  a.analysis_type = id.analysis_type;
  a.run_index = id.run_index;
  a.record_schema_version = rec.provenance.schema_version;

  const auto& in = rec.instrument;
  a.mass_spectrometer = in.mass_spectrometer;
  a.analyst = in.analyst;
  if (!in.laboratory.empty()) a.laboratory = in.laboratory;
  if (!in.extract_device.empty() && in.extract_device != "(none)") a.extract_device = in.extract_device;

  a.roots.signals = ps::Uuid::v5(analysis, "signals");
  a.roots.intercepts = ps::Uuid::v5(analysis, "intercepts");
  a.roots.baselines = ps::Uuid::v5(analysis, "baselines");
  a.roots.blanks = ps::Uuid::v5(analysis, "blanks");
  a.roots.icfactors = ps::Uuid::v5(analysis, "icfactors");
  a.roots.tags = ps::Uuid::v5(analysis, "tags");
  // A failed run is kept, but left out as any invalid analysis is (spec 4.5).
  if (rec.provenance.status == record::RecordStatus::Failed) {
    a.roots.tag.name = "invalid";
    if (!rec.provenance.failure.empty()) a.roots.tag.note = rec.provenance.failure;
  }
  return {};
}

// ---- extraction

void fill_extraction(const record::Extraction& ex, ps::AnalysisIngest& a) {
  const auto& spec = ex.spec;
  auto& out = a.extraction;
  out.extract_value = finite(spec.value);
  if (!spec.units.empty()) out.extract_units = spec.units;
  out.extract_duration = finite(spec.duration);
  out.cleanup_duration = finite(spec.cleanup);
  if (!spec.pattern.empty()) out.pattern = spec.pattern;
  if (spec.cryo_temperature) out.cryo_temperature = finite(*spec.cryo_temperature);
  if (ex.actuals.beam_diameter > 0) out.beam_diameter = finite(ex.actuals.beam_diameter);
  for (const int p : spec.positions) {
    ps::MeasuredPositionRow row;
    row.position = p;
    a.measured_positions.push_back(row);
  }
}

// ---- series: blobs, signal refs, isotopes, detectors

Result<void> fill_series(const record::AnalysisRecord& rec, const ps::Uuid& analysis, const ps::Uuid& client,
                         std::vector<ps::IngestItem>& blobs, ps::AnalysisIngest& a) {
  // Checked on the record's own series: a detector's baseline series are
  // joined below, and a length that does not match must not be hidden in one.
  for (const auto& s : rec.data.series) {
    const auto& tr = s.trace;
    if (tr.t.size() != tr.v.size() || (!tr.sigma.empty() && tr.sigma.size() != tr.t.size()))
      return fail(ErrorKind::Config, "series " + s.iso + "/" + s.det + "/" + s.kind + " has mismatched lengths");
  }

  std::set<Sha256Digest> seen;
  std::set<std::pair<std::string, std::string>> isotopes;
  std::set<std::string> detectors;
  std::map<std::pair<std::string, std::string>, std::string> keys;  // (kind, key) -> detector

  // Under an analysis' names (record/names.hpp): an isotope measured on
  // several detectors is "<detector>:<isotope>", and a detector's baseline
  // series are one. What is left sharing a key no measurement makes: two
  // series of one kind for one isotope on one detector.
  for (const auto& s : record::named_series(rec.data)) {
    const auto& tr = s.trace;
    if (const auto [it, fresh] = keys.emplace(std::make_pair(s.kind, s.key), s.detector); !fresh)
      return fail(ErrorKind::Config, "two " + s.kind + " series for " + s.key + " (detectors " + it->second + ", " +
                                         s.detector + "): the store keeps one");

    ps::BlobIngest blob;
    blob.n_points = static_cast<int>(tr.t.size());
    if (tr.sigma.empty()) {
      std::vector<ps::TvPoint> pts;
      pts.reserve(tr.t.size());
      for (std::size_t i = 0; i < tr.t.size(); ++i) pts.push_back({tr.t[i], tr.v[i]});
      blob.codec = std::string(ps::kCodecTv);
      blob.bytes = ps::encode_tv(pts);
    } else {
      std::vector<ps::TvsPoint> pts;
      pts.reserve(tr.t.size());
      for (std::size_t i = 0; i < tr.t.size(); ++i) pts.push_back({tr.t[i], tr.v[i], tr.sigma[i]});
      blob.codec = std::string(ps::kCodecTvs);
      blob.bytes = ps::encode_tvs(pts);
    }
    const auto sha = ps::blob_sha256(blob.codec, blob.bytes);
    if (seen.insert(sha).second) {
      ps::IngestItem item;
      item.item = ps::Uuid::v5(analysis, "blob:" + to_hex(sha));
      item.payload_sha256 = sha;
      item.client = client;
      item.body = std::move(blob);
      blobs.push_back(std::move(item));
    }

    ps::SignalRefRow ref;
    ref.series_kind = s.kind;
    ref.series_key = s.key;
    ref.detector = s.detector;
    ref.blob_sha = sha;
    ref.n_points = static_cast<int>(tr.t.size());
    a.roots.signal_refs.push_back(std::move(ref));

    if (s.kind == "signal") isotopes.emplace(s.key, s.detector);
    detectors.insert(s.detector);
  }

  for (const auto& [iso, det] : isotopes) a.isotopes.push_back({iso, det, {}, {}, {}, {}});
  for (const auto& det : detectors) {
    ps::DetectorRow row;
    row.detector = det;
    if (const auto d = rec.spectrometer.deflections.find(det); d != rec.spectrometer.deflections.end())
      row.deflection = finite(d->second);
    if (const auto g = rec.spectrometer.gains.find(det); g != rec.spectrometer.gains.end()) row.gain_used = finite(g->second);
    a.detectors.push_back(std::move(row));
  }
  return {};
}

// ---- fits: the names and the outlier JSON the store reads back

std::string outliers_json(const reduction::OutlierSpec& o) {
  json j;
  j["filter_outliers"] = o.enabled ? 1 : 0;
  j["iterations"] = o.iterations;
  j["std_devs"] = o.std_devs;
  return dump(j);
}

const char* error_type_of(const reduction::FitSpec& fit) { return fit.error == reduction::ErrorType::Sd ? "SD" : "SEM"; }

Result<void> fill_fits(const record::Results& res, const record::Data& data, ps::AnalysisIngest& a) {
  // The record keeps an intercept under the acquisition's name; the row is
  // under the analysis' (record/names.hpp), on that isotope's detector.
  for (const auto& [name, r] : res.intercepts) {
    const auto named = record::intercept_name(data, name);
    if (!named) return fail(ErrorKind::Config, "intercept " + name + " has no series");
    ps::InterceptRow row;
    row.isotope = named->key;
    row.detector = named->detector;
    row.value = finite(r.intercept.value);
    row.error = finite(r.intercept.error);
    row.n = static_cast<int>(r.intercept.n_used);
    row.fit = std::string(reduction::to_string(r.fit.kind));
    row.error_type = error_type_of(r.fit);
    row.filter_outliers_json = outliers_json(r.fit.outliers);
    a.roots.intercepts_rows.push_back(std::move(row));
  }
  for (const auto& [det, r] : res.baselines) {
    ps::BaselineRow row;
    row.detector = det;
    row.value = finite(r.value);
    row.error = finite(r.error);
    row.fit = std::string(reduction::to_string(r.fit.kind));
    row.error_type = error_type_of(r.fit);
    row.filter_outliers_json = outliers_json(r.fit.outliers);
    a.roots.baselines_rows.push_back(std::move(row));
  }
  for (const auto& [det, v] : res.icfactors) {
    ps::IcFactorRow row;
    row.detector = det;
    row.value = finite(v);
    row.error = 0.0;
    a.roots.icfactors_rows.push_back(std::move(row));
  }
  return {};
}

// ---- spectrometer, scripts and the JSON-only satellites

void fill_spectrometer(const record::AnalysisRecord& rec, ps::AnalysisIngest& a) {
  const auto& sp = rec.spectrometer;
  ps::SpectrometerSnapshot snap;
  snap.gains_json = dump(json(sp.gains));
  snap.deflections_json = dump(json(sp.deflections));
  json settings = json(sp.source_params);
  settings["integration_time"] = sp.integration_time;
  snap.settings_json = dump(settings);
  snap.spectrometer_json = "{}";
  a.spectrometer_snapshot = std::move(snap);
  if (!rec.measurement.plan.effective_plan_toml.empty()) a.scripts.measurement = rec.measurement.plan.effective_plan_toml;
}

json to_json(const record::InstalledConditional& c) {
  return {{"id", c.id},
          {"name", c.name},
          {"kind", c.kind},
          {"level", c.level},
          {"location", c.location},
          {"check", c.check},
          {"start", c.start},
          {"frequency", c.frequency},
          {"ntrips", c.ntrips},
          {"window", c.window},
          {"mapper", c.mapper},
          {"analysis_types", c.analysis_types},
          {"abbreviated_count_ratio", c.abbreviated_count_ratio},
          {"action", c.action},
          {"resume", c.resume},
          {"truncate", c.truncate},
          {"terminate", c.terminate}};
}

json to_json(const record::TrippedConditional& c) {
  return {{"id", c.id},       {"name", c.name},   {"kind", c.kind},   {"check", c.check}, {"action", c.action},
          {"reading", c.reading}, {"count", c.count}, {"t", c.t}, {"value", c.value}, {"context", c.context}};
}

void fill_meta(const record::AnalysisRecord& rec, ps::AnalysisIngest& a) {
  ps::AnalysisMetaRow meta;
  bool any = false;
  const auto set = [&](std::optional<std::string>& slot, std::string text) {
    slot = std::move(text);
    any = true;
  };

  const auto& cond = rec.conditionals;
  if (!cond.installed.empty()) {
    json arr = json::array();
    for (const auto& c : cond.installed) arr.push_back(to_json(c));
    set(meta.conditionals_json, dump(arr));
  }
  if (!cond.tripped.empty()) {
    json arr = json::array();
    for (const auto& c : cond.tripped) arr.push_back(to_json(c));
    set(meta.tripped_conditional_json, dump(arr));
  }
  if (!rec.results.whiff.empty()) set(meta.whiff_result_json, dump(json(rec.results.whiff)));

  const auto& act = rec.extraction.actuals;
  if (!act.grain_polygons.empty()) {
    json polys = json::array();
    for (const auto& poly : act.grain_polygons) {
      json pts = json::array();
      for (const auto& [x, y] : poly) pts.push_back(json::array({x, y}));
      polys.push_back(std::move(pts));
    }
    set(meta.grain_polygons_json, dump(polys));
  }
  if (act.pipette_counts > 0) set(meta.pipette_counts_json, dump(json(act.pipette_counts)));

  const auto& in = rec.instrument;
  if (!in.software_version.empty() || !in.software_git_sha.empty() || in.simulated) {
    json software{{"software_version", in.software_version}, {"software_git_sha", in.software_git_sha}};
    // A simulator's run says so where it is stored; an instrument's has no such key.
    if (in.simulated) software["simulated"] = true;
    set(meta.software_json, dump(software));
  }

  if (!rec.identity.queue_uuid.empty()) set(meta.queue_names_json, dump(json::array({rec.identity.queue_uuid})));

  if (any) a.meta = std::move(meta);
}

}  // namespace

std::optional<int> increment_of(std::string_view step) {
  if (step.empty()) return -1;
  if (step.size() > 6) return std::nullopt;  // far past any real step; keeps the sum in int
  int n = 0;
  for (const char c : step) {
    const char u = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    if (u < 'A' || u > 'Z') return std::nullopt;
    n = n * 26 + (u - 'A' + 1);
  }
  return n - 1;
}

Result<RunIngest> to_ingest(const record::AnalysisRecord& rec, ps::Uuid client) {
  const auto analysis = ps::Uuid::parse(rec.identity.uuid);
  if (!analysis) return fail(ErrorKind::Config, "the record's uuid does not parse: " + rec.identity.uuid);
  const auto sha = digest_of(rec.provenance.sha);
  if (!sha) return fail(ErrorKind::Config, "the record was not finalized (no sha)");

  RunIngest out;
  ps::AnalysisIngest a;
  if (auto r = fill_identity(rec, *analysis, a); !r) return fail(r.error());
  a.record_sha256 = *sha;
  fill_extraction(rec.extraction, a);
  if (auto r = fill_series(rec, *analysis, client, out.blobs, a); !r) return fail(r.error());
  if (auto r = fill_fits(rec.results, rec.data, a); !r) return fail(r.error());
  fill_spectrometer(rec, a);
  fill_meta(rec, a);

  out.analysis.item = ps::Uuid::v5(*analysis, "item");
  out.analysis.payload_sha256 = *sha;
  out.analysis.client = client;
  out.analysis.body = std::move(a);
  return out;
}

}  // namespace pychron::experiment::store
