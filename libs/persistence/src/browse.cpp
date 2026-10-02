// Browsing reads (data browsing and visualization design, section 9.2):
// filtered, keyset-paged analysis rows, facet values, analysis detail and raw
// blobs.

#include <algorithm>
#include <cctype>

#include "sql/browse.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {

namespace {

const QString& facet_column(BrowseFacet f) {
  switch (f) {
    case BrowseFacet::AnalysisType:
      return sql::kColAnalysisType;
    case BrowseFacet::MassSpectrometer:
      return sql::kColMassSpectrometer;
    case BrowseFacet::ExtractDevice:
      return sql::kColExtractDevice;
    case BrowseFacet::Project:
      return sql::kColProject;
    case BrowseFacet::PrincipalInvestigator:
      return sql::kColPrincipalInvestigator;
    case BrowseFacet::Sample:
      return sql::kColSample;
    case BrowseFacet::Material:
      return sql::kColMaterial;
    case BrowseFacet::Identifier:
      return sql::kColIdentifier;
    case BrowseFacet::Irradiation:
      return sql::kColIrradiation;
    case BrowseFacet::Level:
      return sql::kColLevel;
    case BrowseFacet::Load:
      return sql::kColLoad;
    case BrowseFacet::Repository:
      break;
  }
  return sql::kColAnalysisType;  // repository has its own query
}

// "abc_%" -> "abc\_\%%": a case-insensitive LIKE prefix.
QString like_prefix(const std::string& text) {
  std::string out;
  for (char c : text) {
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == '%' || l == '_' || l == '\\') out += '\\';
    out += l;
  }
  out += '%';
  return qs(out);
}

struct Where {
  QStringList clauses;
  Bindings bindings;

  QString sql() const { return clauses.isEmpty() ? QString() : QStringLiteral("WHERE ") + clauses.join(QStringLiteral(" AND ")); }
};

Result<Where> build_where(Db& db, Dialect dialect, const BrowseFilter& f, std::optional<BrowseFacet> ignore) {
  Where w;
  auto list = [&](BrowseFacet facet, const std::vector<std::string>& values) {
    if (values.empty() || ignore == facet) return;
    if (facet == BrowseFacet::Repository) {
      w.clauses << sql::kBrowseRepositoryIn.arg(sql::placeholders(static_cast<qsizetype>(values.size())));
    } else {
      w.clauses << sql::kBrowseIn.arg(facet_column(facet), sql::placeholders(static_cast<qsizetype>(values.size())));
    }
    for (const auto& v : values) w.bindings << qv(v);
  };
  list(BrowseFacet::Identifier, f.identifiers);
  list(BrowseFacet::Sample, f.samples);
  list(BrowseFacet::Project, f.projects);
  list(BrowseFacet::PrincipalInvestigator, f.principal_investigators);
  list(BrowseFacet::Material, f.materials);
  list(BrowseFacet::AnalysisType, f.analysis_types);
  list(BrowseFacet::MassSpectrometer, f.mass_spectrometers);
  list(BrowseFacet::ExtractDevice, f.extract_devices);
  list(BrowseFacet::Load, f.loads);
  list(BrowseFacet::Irradiation, f.irradiations);
  list(BrowseFacet::Level, f.levels);
  list(BrowseFacet::Repository, f.repositories);
  if (!f.text.empty()) {
    w.clauses << sql::kBrowseText;
    const QString p = like_prefix(f.text);
    w.bindings << p << p << p;
  }
  std::optional<UtcTime> from = f.from;
  if (f.last_hours) {
    auto newest = db.select_one(sql::kNewestAnalysis.arg(sql::ts(dialect, QStringLiteral("timestamp_utc"))));
    if (!newest) return fail(newest.error());
    if (*newest) {
      UtcTime since = to_time((*newest)->value("ts"));
      since.micros -= static_cast<std::int64_t>(*f.last_hours * 3600.0 * 1e6);
      if (!from || since > *from) from = since;
    }
  }
  if (from) {
    w.clauses << sql::kBrowseSince;
    w.bindings << qv(*from);
  }
  if (f.to) {
    w.clauses << sql::kBrowseTo;
    w.bindings << qv(*f.to);
  }
  if (!f.exclude_tags.empty()) {
    w.clauses << sql::kBrowseTagNotIn.arg(sql::placeholders(static_cast<qsizetype>(f.exclude_tags.size())));
    for (const auto& t : f.exclude_tags) w.bindings << qv(t);
  }
  return w;
}

BrowseRow row_from(const Row& r) {
  BrowseRow b;
  auto& s = b.summary;
  s.uuid = to_uuid(r.value("uuid"));
  s.runid = to_std(r.value("runid_text"));
  s.identifier = to_std(r.value("identifier"));
  s.aliquot = r.value("aliquot").toInt();
  s.increment = r.value("increment").toInt();
  s.provisional = r.value("provisional").toBool();
  s.analysis_type = to_std(r.value("analysis_type"));
  s.timestamp = to_time(r.value("ts"));
  s.mass_spectrometer = to_std(r.value("mass_spectrometer"));
  s.signals_state = to_std(r.value("signals_state"));
  b.sample = to_std(r.value("sample"));
  b.project = to_std(r.value("project"));
  b.material = to_std(r.value("material"));
  b.principal_investigator = to_std(r.value("principal_investigator"));
  b.extract_device = to_std(r.value("extract_device"));
  b.load = to_std(r.value("load_name"));
  b.irradiation = to_std(r.value("irradiation"));
  b.level = to_std(r.value("level"));
  b.repository = to_std(r.value("repository"));
  b.tag = to_std(r.value("tag"));
  b.position = opt_int(r.value("position"));
  b.extract_value = opt_double(r.value("extract_value"));
  b.extract_units = to_std(r.value("extract_units"));
  return b;
}

}  // namespace

Result<BrowseResult> browse(Db& db, Dialect dialect, const BrowseRequest& req) {
  if (req.limit <= 0) return fail(ErrorKind::Protocol, "browse: limit must be positive");
  auto where = build_where(db, dialect, req.filter, std::nullopt);
  if (!where) return fail(where.error());
  BrowseResult out;
  if (req.count_total) {
    auto n = db.select_one(sql::kBrowseCount + sql::kBrowseFrom + where->sql(), where->bindings);
    if (!n) return fail(n.error());
    if (*n) out.total = (*n)->value("n").toLongLong();
  }
  Where paged = *where;
  if (req.after) {
    paged.clauses << sql::kBrowseBefore;
    paged.bindings << qv(req.after->timestamp) << qv(req.after->timestamp) << qv(req.after->uuid);
  }
  Bindings b = paged.bindings;
  b << req.limit + 1;
  auto rows = db.select(sql::kBrowseSelect.arg(sql::ts(dialect, QStringLiteral("a.timestamp_utc"))) + sql::kBrowseFrom +
                            paged.sql() + sql::kBrowseOrder,
                        b);
  if (!rows) return fail(rows.error());
  const bool more = static_cast<int>(rows->size()) > req.limit;
  if (more) rows->pop_back();
  for (const auto& r : *rows) out.rows.push_back(row_from(r));
  if (more && !out.rows.empty())
    out.next = BrowseCursorKey{out.rows.back().summary.timestamp, out.rows.back().summary.uuid};
  return out;
}

Result<std::vector<std::string>> facet(Db& db, Dialect dialect, BrowseFacet f, const BrowseFilter& filter) {
  auto where = build_where(db, dialect, filter, f);
  if (!where) return fail(where.error());
  Result<std::vector<Row>> rows = std::vector<Row>{};
  if (f == BrowseFacet::Repository) {
    rows = db.select(sql::kRepositoryFacet.arg(sql::kBrowseFrom, where->sql()), where->bindings);
  } else {
    Where w = *where;
    w.clauses << sql::kFacetNotNull.arg(facet_column(f));
    rows = db.select(sql::kFacetSelect.arg(facet_column(f)) + sql::kBrowseFrom + w.sql() + sql::kFacetTail, w.bindings);
  }
  if (!rows) return fail(rows.error());
  std::vector<std::string> out;
  for (const auto& r : *rows) {
    auto v = to_std(r.value("v"));
    if (!v.empty()) out.push_back(std::move(v));
  }
  return out;
}

Result<std::optional<AnalysisDetail>> load_analysis_detail(Db& db, Dialect dialect, Uuid analysis) {
  auto head = db.select_one(sql::kBrowseSelect.arg(sql::ts(dialect, QStringLiteral("a.timestamp_utc"))) +
                                sql::kBrowseFrom + QStringLiteral("WHERE a.uuid = ?"),
                            {qv(analysis)});
  if (!head) return fail(head.error());
  if (!*head) return std::optional<AnalysisDetail>{};
  AnalysisDetail d;
  d.row = row_from(**head);
  auto det = db.select_one(sql::kAnalysisDetail, {qv(analysis)});
  if (!det) return fail(det.error());
  if (*det) {
    const Row& r = **det;
    auto& e = d.extraction;
    e.extract_value = opt_double(r.value("extract_value"));
    e.extract_units = opt_str(r.value("extract_units"));
    e.extract_duration = opt_double(r.value("extract_duration"));
    e.cleanup_duration = opt_double(r.value("cleanup_duration"));
    e.pre_cleanup = opt_double(r.value("pre_cleanup"));
    e.post_cleanup = opt_double(r.value("post_cleanup"));
    e.cryo_temperature = opt_double(r.value("cryo_temperature"));
    e.weight = opt_double(r.value("weight"));
    e.beam_diameter = opt_double(r.value("beam_diameter"));
    e.pattern = opt_str(r.value("pattern"));
    e.ramp_duration = opt_double(r.value("ramp_duration"));
    e.ramp_rate = opt_double(r.value("ramp_rate"));
    e.light_value = opt_double(r.value("light_value"));
    e.tray = opt_str(r.value("tray"));
    d.analyst = opt_str(r.value("analyst"));
    d.environmental_json = opt_str(r.value("environmental"));
  }
  auto isos = db.select(sql::kAnalysisIsotopes, {qv(analysis)});
  if (!isos) return fail(isos.error());
  for (const auto& r : *isos)
    d.isotopes.push_back({to_std(r.value("isotope")), to_std(r.value("detector")), opt_str(r.value("units")),
                          opt_str(r.value("detector_serial")), opt_str(r.value("classification")),
                          opt_double(r.value("classification_probability"))});
  auto dets = db.select(sql::kAnalysisDetectors, {qv(analysis)});
  if (!dets) return fail(dets.error());
  for (const auto& r : *dets)
    d.detectors.push_back({to_std(r.value("detector")), opt_double(r.value("deflection")), opt_double(r.value("gain_used"))});
  auto pcs = db.select(sql::kAnalysisPeakCenters, {qv(analysis)});
  if (!pcs) return fail(pcs.error());
  for (const auto& r : *pcs) {
    PeakCenterRow p;
    p.detector = to_std(r.value("detector"));
    p.reference_detector = opt_str(r.value("reference_detector"));
    p.reference_isotope = opt_str(r.value("reference_isotope"));
    p.interpolation = opt_str(r.value("interpolation"));
    p.low_dac = opt_double(r.value("low_dac"));
    p.center_dac = opt_double(r.value("center_dac"));
    p.high_dac = opt_double(r.value("high_dac"));
    p.low_signal = opt_double(r.value("low_signal"));
    p.center_signal = opt_double(r.value("center_signal"));
    p.high_signal = opt_double(r.value("high_signal"));
    p.resolution = opt_double(r.value("resolution"));
    p.low_resolving_power = opt_double(r.value("low_resolving_power"));
    p.high_resolving_power = opt_double(r.value("high_resolving_power"));
    if (!r.value("points_blob_sha").isNull()) p.points_blob_sha = to_digest(r.value("points_blob_sha"));
    d.peak_centers.push_back(std::move(p));
  }
  return std::optional<AnalysisDetail>{std::move(d)};
}

Result<std::optional<BlobData>> load_blob(Db& db, const Sha256Digest& sha) {
  auto row = db.select_one(sql::kBlobByShaFull, {qv(sha)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<BlobData>{};
  BlobData b;
  b.codec = to_std((*row)->value("codec"));
  b.n_points = opt_int((*row)->value("n_points"));
  const QByteArray bytes = (*row)->value("bytes").toByteArray();
  b.bytes.assign(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                 reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
  return std::optional<BlobData>{std::move(b)};
}

}  // namespace pychron::persistence::detail
