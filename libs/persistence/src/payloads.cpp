// Payload rows per revision kind (DVC schema spec, section 4.1). Payload
// tables are insert-only; a revision's rows are written once, in the same
// transaction as the revision.

#include <map>

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {
namespace {

void put_manual(Row& r, const ManualOverride& m) {
  r["use_manual_value"] = m.use_value;
  r["manual_value"] = qv(m.value);
  r["use_manual_error"] = m.use_error;
  r["manual_error"] = qv(m.error);
}

ManualOverride get_manual(const Row& r) {
  return {r.value("use_manual_value").toBool(), opt_double(r.value("manual_value")),
          r.value("use_manual_error").toBool(), opt_double(r.value("manual_error"))};
}

QList<QVariantMap> reference_rows(Uuid revision, const char* key_column, const std::string& key,
                                  const std::vector<ReferenceRow>& refs) {
  QList<QVariantMap> rows;
  for (const auto& ref : refs) {
    Row r;
    r["revision_uuid"] = qv(revision);
    r[key_column] = qv(key);
    r["ordinal"] = ref.ordinal;
    r["ref_analysis_uuid"] = qv(ref.ref_analysis);
    r["record_id"] = qv(ref.record_id);
    r["exclude"] = ref.exclude;
    rows.push_back(std::move(r));
  }
  return rows;
}

struct RefWriter;

struct Writer {
  Db& db;
  Uuid revision;
  Uuid subject;

  Row base() const {
    Row r;
    r["revision_uuid"] = qv(revision);
    return r;
  }

  Result<void> operator()(const Intercepts& rows) const {
    QList<QVariantMap> out;
    for (const auto& i : rows) {
      Row r = base();
      r["isotope"] = qv(i.isotope);
      r["detector"] = qv(i.detector);
      r["value"] = qv(i.value);
      r["error"] = qv(i.error);
      r["fit"] = qv(i.fit);
      r["error_type"] = qv(i.error_type);
      r["n"] = qv(i.n);
      r["fn"] = qv(i.fn);
      r["include_baseline_error"] = qv(i.include_baseline_error);
      r["filter_outliers"] = qv(i.filter_outliers_json);
      r["user_excluded"] = qv(i.user_excluded_json);
      r["outlier_excluded"] = qv(i.outlier_excluded_json);
      r["reviewed"] = i.reviewed;
      put_manual(r, i.manual);
      r["extra"] = qv(i.extra_json);
      out.push_back(std::move(r));
    }
    return db.insert_many("intercept_value", out);
  }

  Result<void> operator()(const Baselines& rows) const {
    QList<QVariantMap> out;
    for (const auto& b : rows) {
      Row r = base();
      r["detector"] = qv(b.detector);
      r["value"] = qv(b.value);
      r["error"] = qv(b.error);
      r["fit"] = qv(b.fit);
      r["error_type"] = qv(b.error_type);
      r["n"] = qv(b.n);
      r["fn"] = qv(b.fn);
      r["filter_outliers"] = qv(b.filter_outliers_json);
      r["user_excluded"] = qv(b.user_excluded_json);
      r["modifier_value"] = qv(b.modifier_value);
      r["modifier_error"] = qv(b.modifier_error);
      r["reviewed"] = b.reviewed;
      put_manual(r, b.manual);
      r["extra"] = qv(b.extra_json);
      out.push_back(std::move(r));
    }
    return db.insert_many("baseline_value", out);
  }

  Result<void> operator()(const Blanks& rows) const {
    QList<QVariantMap> out;
    QList<QVariantMap> refs;
    for (const auto& b : rows) {
      Row r = base();
      r["isotope"] = qv(b.isotope);
      r["value"] = qv(b.value);
      r["error"] = qv(b.error);
      r["fit"] = qv(b.fit);
      r["error_type"] = qv(b.error_type);
      r["reviewed"] = b.reviewed;
      put_manual(r, b.manual);
      r["extra"] = qv(b.extra_json);
      out.push_back(std::move(r));
      refs += reference_rows(revision, "isotope", b.isotope, b.references);
    }
    if (auto r = db.insert_many("blank_value", out); !r) return r;
    return db.insert_many("blank_reference", refs);
  }

  Result<void> operator()(const IcFactors& rows) const {
    QList<QVariantMap> out;
    QList<QVariantMap> refs;
    for (const auto& f : rows) {
      Row r = base();
      r["detector"] = qv(f.detector);
      r["value"] = qv(f.value);
      r["error"] = qv(f.error);
      r["fit"] = qv(f.fit);
      r["reviewed"] = f.reviewed;
      r["reference_detector"] = qv(f.reference_detector);
      r["standard_ratio"] = qv(f.standard_ratio);
      r["discrimination"] = f.discrimination;
      r["source_correction"] = f.source_correction;
      r["reference_data"] = qv(f.reference_data_json);
      put_manual(r, f.manual);
      r["extra"] = qv(f.extra_json);
      out.push_back(std::move(r));
      refs += reference_rows(revision, "detector", f.detector, f.references);
    }
    if (auto r = db.insert_many("icfactor_value", out); !r) return r;
    return db.insert_many("icfactor_reference", refs);
  }

  Result<void> operator()(const SignalRefs& rows) const {
    QList<QVariantMap> out;
    for (const auto& s : rows) {
      Row r = base();
      r["series_kind"] = qv(s.series_kind);
      r["series_key"] = qv(s.series_key);
      r["detector"] = qv(s.detector);
      r["blob_sha"] = qv(s.blob_sha);
      r["n_points"] = qv(s.n_points);
      r["start_index"] = qv(s.start_index);
      r["end_index"] = qv(s.end_index);
      out.push_back(std::move(r));
    }
    return db.insert_many("signal_ref", out);
  }

  Result<void> operator()(const TagValue& t) const {
    Row r = base();
    r["name"] = qv(t.name);
    r["note"] = qv(t.note);
    r["subgroup"] = qv(t.subgroup_json);
    return db.insert("tag_value", r);
  }

  Result<void> operator()(const AnnotationValue& a) const {
    Row r = base();
    r["comment"] = qv(a.comment);
    return db.insert("annotation_value", r);
  }

  Result<void> operator()(const RefPins& rows) const {
    QList<QVariantMap> out;
    for (const auto& p : rows) {
      Row r = base();
      r["ref_object_uuid"] = qv(p.ref_object);
      r["ref_revision_uuid"] = qv(p.ref_revision);
      out.push_back(std::move(r));
    }
    return db.insert_many("refpin_value", out);
  }

  Result<void> operator()(const CosmogenicValue& c) const {
    Row r = base();
    r["doc"] = qv(c.doc_json);
    return db.insert("cosmogenic_value", r);
  }

  Result<void> operator()(const IdentityValue& v) const {
    Row r = base();
    r["identifier_uuid"] = qv(v.identifier);
    r["aliquot"] = v.aliquot;
    r["increment"] = v.increment;
    r["reason"] = qv(v.reason);
    return db.insert("identity_value", r);
  }

  Result<void> operator()(const InterpretedAgeValue& v) const {
    Row r = base();
    r["age"] = qv(v.age);
    r["age_err"] = qv(v.age_err);
    r["age_kind"] = qv(v.age_kind);
    r["kca"] = qv(v.kca);
    r["kca_err"] = qv(v.kca_err);
    r["mswd"] = qv(v.mswd);
    r["nanalyses"] = qv(v.nanalyses);
    r["doc"] = qv(v.doc_json);
    if (auto ins = db.insert("ia_value", r); !ins) return ins;
    QList<QVariantMap> members;
    for (const auto& m : v.members) {
      Row x = base();
      x["analysis_uuid"] = qv(m.analysis);
      x["record_id"] = qv(m.record_id);
      x["plateau_step"] = qv(m.plateau_step);
      x["tag"] = qv(m.tag);
      members.push_back(std::move(x));
    }
    return db.insert_many("ia_member", members);
  }

  Result<void> operator()(const RefPayload& p) const;
};

struct RefWriter {
  const Writer& w;

  Result<void> operator()(const FluxValue& f) const {
    Row r = w.base();
    r["j"] = qv(f.j);
    r["j_err"] = qv(f.j_err);
    r["mean_j"] = qv(f.mean_j);
    r["mean_j_err"] = qv(f.mean_j_err);
    r["mean_j_mswd"] = qv(f.mean_j_mswd);
    r["position_jerr"] = qv(f.position_jerr);
    r["lambda_k_total"] = qv(f.lambda_k_total);
    r["lambda_k_total_err"] = qv(f.lambda_k_total_err);
    r["monitor_name"] = qv(f.monitor_name);
    r["monitor_material"] = qv(f.monitor_material);
    r["monitor_age"] = qv(f.monitor_age);
    r["monitor_age_err"] = qv(f.monitor_age_err);
    r["options"] = qv(f.options_json);
    r["extra"] = qv(f.extra_json);
    if (auto ins = w.db.insert("flux_value", r); !ins) return ins;
    QList<QVariantMap> rows;
    for (const auto& a : f.analyses) {
      Row x = w.base();
      x["analysis_uuid"] = qv(a.analysis);
      x["record_id"] = qv(a.record_id);
      x["is_omitted"] = a.is_omitted;
      rows.push_back(std::move(x));
    }
    return w.db.insert_many("flux_value_analysis", rows);
  }

  Result<void> operator()(const LevelZValue& v) const {
    Row r = w.base();
    r["z"] = qv(v.z);
    return w.db.insert("level_z_value", r);
  }

  Result<void> operator()(const ProductionValue& v) const {
    Row r = w.base();
    r["reactor"] = qv(v.reactor);
    r["note"] = qv(v.note);
    if (auto ins = w.db.insert("production_meta", r); !ins) return ins;
    QList<QVariantMap> rows;
    for (const auto& k : v.ratios) {
      Row x = w.base();
      x["key"] = qv(k.key);
      x["value"] = k.value;
      x["error"] = k.error;
      rows.push_back(std::move(x));
    }
    return w.db.insert_many("production_value", rows);
  }

  Result<void> operator()(const LevelProductionValue& v) const {
    Row r = w.base();
    r["production_ref_uuid"] = qv(v.production);
    r["note"] = qv(v.note);
    return w.db.insert("level_production_value", r);
  }

  Result<void> operator()(const ChronologyValue& v) const {
    QList<QVariantMap> rows;
    for (const auto& d : v.doses) {
      Row x = w.base();
      x["ordinal"] = d.ordinal;
      x["power"] = d.power;
      x["start_utc"] = qv(d.start);
      x["end_utc"] = qv(d.end);
      rows.push_back(std::move(x));
    }
    return w.db.insert_many("chronology_dose", rows);
  }

  Result<void> operator()(const GainsValue& v) const {
    QList<QVariantMap> rows;
    for (const auto& g : v.gains) {
      Row x = w.base();
      x["detector"] = qv(g.detector);
      x["gain"] = g.gain;
      rows.push_back(std::move(x));
    }
    return w.db.insert_many("detector_gain", rows);
  }

  Result<void> operator()(const SensitivityValue& v) const {
    Row r = w.base();
    r["sensitivity"] = v.sensitivity;
    r["create_date_utc"] = qv(v.create_date);
    r["extra"] = qv(v.extra_json);
    return w.db.insert("sensitivity_value", r);
  }

  Result<void> operator()(const HolderValue& v) const {
    Row r = w.base();
    r["shape"] = qv(v.shape);
    r["radius"] = qv(v.radius);
    r["has_hole_numbers"] = v.has_hole_numbers;
    if (auto ins = w.db.insert("holder_meta", r); !ins) return ins;
    QList<QVariantMap> rows;
    for (const auto& h : v.holes) {
      Row x = w.base();
      x["ordinal"] = h.ordinal;
      x["hole_id"] = qv(h.hole_id);
      x["x"] = h.x;
      x["y"] = h.y;
      x["radius"] = qv(h.radius);
      rows.push_back(std::move(x));
    }
    return w.db.insert_many("holder_hole", rows);
  }

  Result<void> operator()(const ScriptValue& v) const {
    auto sha = put_script_text(w.db, v.body);
    if (!sha) return fail(sha.error());
    Row r = w.base();
    r["script_sha"] = qv(*sha);
    return w.db.insert("script_version", r);
  }

  Result<void> operator()(const DocumentValue& v) const {
    Row r = w.base();
    r["content_text"] = qv(v.content_text);
    r["content_json"] = qv(v.content_json);
    return w.db.insert("ref_document", r);
  }
};

Result<void> Writer::operator()(const RefPayload& p) const {
  auto row = db.select_one(sql::kRefTypeOfObject, {qv(subject)});
  if (!row) return fail(row.error());
  if (!*row) return fail(ErrorKind::Protocol, "unknown ref_object " + subject.str());
  const auto type = parse_ref_type(to_std((*row)->value("ref_type")));
  if (!type || !ref_payload_matches(*type, p))
    return fail(ErrorKind::Protocol, "payload does not match ref_type '" + to_std((*row)->value("ref_type")) + "'");
  return std::visit(RefWriter{*this}, p);
}

Result<RefPayload> read_ref_payload(Db& db, Uuid revision) {
  const Bindings b{qv(revision)};
  auto t = db.select_one(sql::kRefTypeOfRevision, b);
  if (!t) return fail(t.error());
  if (!*t) return fail(ErrorKind::Protocol, "value revision without a ref_object");
  const auto type = parse_ref_type(to_std((*t)->value("ref_type")));
  if (!type) return fail(ErrorKind::Protocol, "unknown ref_type");
  auto one = [&](const QString& q) -> Result<Row> {
    auto r = db.select_one(q, b);
    if (!r) return fail(r.error());
    if (!*r) return fail(ErrorKind::Protocol, "reference revision has no payload row");
    return **r;
  };
  auto many = [&](const QString& q) { return db.select(q, b); };
  switch (*type) {
    case RefType::FluxPosition: {
      auto r = one(sql::kFlux);
      if (!r) return fail(r.error());
      FluxValue f;
      f.j = opt_double(r->value("j"));
      f.j_err = opt_double(r->value("j_err"));
      f.mean_j = opt_double(r->value("mean_j"));
      f.mean_j_err = opt_double(r->value("mean_j_err"));
      f.mean_j_mswd = opt_double(r->value("mean_j_mswd"));
      f.position_jerr = opt_double(r->value("position_jerr"));
      f.lambda_k_total = opt_double(r->value("lambda_k_total"));
      f.lambda_k_total_err = opt_double(r->value("lambda_k_total_err"));
      f.monitor_name = opt_str(r->value("monitor_name"));
      f.monitor_material = opt_str(r->value("monitor_material"));
      f.monitor_age = opt_double(r->value("monitor_age"));
      f.monitor_age_err = opt_double(r->value("monitor_age_err"));
      f.options_json = opt_str(r->value("options"));
      f.extra_json = opt_str(r->value("extra"));
      auto rows = many(sql::kFluxAnalyses);
      if (!rows) return fail(rows.error());
      for (const auto& x : *rows)
        f.analyses.push_back(FluxAnalysis{opt_uuid(x.value("analysis_uuid")), to_std(x.value("record_id")),
                                          x.value("is_omitted").toBool()});
      return RefPayload{std::move(f)};
    }
    case RefType::LevelGeometry: {
      auto r = one(sql::kLevelZ);
      if (!r) return fail(r.error());
      return RefPayload{LevelZValue{opt_double(r->value("z"))}};
    }
    case RefType::Production: {
      auto r = one(sql::kProductionMeta);
      if (!r) return fail(r.error());
      ProductionValue v{opt_str(r->value("reactor")), opt_str(r->value("note")), {}};
      auto rows = many(sql::kProductionValues);
      if (!rows) return fail(rows.error());
      for (const auto& x : *rows)
        v.ratios.push_back(ProductionRatio{to_std(x.value("key")), x.value("value").toDouble(), x.value("error").toDouble()});
      return RefPayload{std::move(v)};
    }
    case RefType::LevelProduction: {
      auto r = one(sql::kLevelProduction);
      if (!r) return fail(r.error());
      return RefPayload{LevelProductionValue{to_uuid(r->value("production_ref_uuid")), opt_str(r->value("note"))}};
    }
    case RefType::Chronology: {
      auto rows = db.select(sql::kChronology.arg(sql::ts(db.dialect(), QStringLiteral("start_utc")),
                                                 sql::ts(db.dialect(), QStringLiteral("end_utc"))),
                            b);
      if (!rows) return fail(rows.error());
      ChronologyValue v;
      for (const auto& x : *rows)
        v.doses.push_back(Dose{x.value("ordinal").toInt(), x.value("power").toDouble(), to_time(x.value("start_ts")),
                               to_time(x.value("end_ts"))});
      return RefPayload{std::move(v)};
    }
    case RefType::Gains: {
      auto rows = many(sql::kGains);
      if (!rows) return fail(rows.error());
      GainsValue v;
      for (const auto& x : *rows) v.gains.push_back(DetectorGain{to_std(x.value("detector")), x.value("gain").toDouble()});
      return RefPayload{std::move(v)};
    }
    case RefType::Sensitivity: {
      auto r = db.select_one(sql::kSensitivity.arg(sql::ts(db.dialect(), QStringLiteral("create_date_utc"))), b);
      if (!r) return fail(r.error());
      if (!*r) return fail(ErrorKind::Protocol, "sensitivity revision has no payload row");
      const QVariant created = (*r)->value("create_ts");
      return RefPayload{SensitivityValue{(*r)->value("sensitivity").toDouble(),
                                         created.isNull() ? std::nullopt : std::optional<UtcTime>(to_time(created)),
                                         opt_str((*r)->value("extra"))}};
    }
    case RefType::IrradiationHolder:
    case RefType::LoadHolder: {
      auto r = one(sql::kHolderMeta);
      if (!r) return fail(r.error());
      HolderValue v{opt_str(r->value("shape")), opt_double(r->value("radius")), r->value("has_hole_numbers").toBool(), {}};
      auto rows = many(sql::kHolderHoles);
      if (!rows) return fail(rows.error());
      for (const auto& x : *rows)
        v.holes.push_back(HolderHole{x.value("ordinal").toInt(), to_std(x.value("hole_id")), x.value("x").toDouble(),
                                     x.value("y").toDouble(), opt_double(x.value("radius"))});
      return RefPayload{std::move(v)};
    }
    case RefType::Script: {
      auto r = one(sql::kScriptVersion);
      if (!r) return fail(r.error());
      return RefPayload{ScriptValue{to_std(r->value("body"))}};
    }
    case RefType::Document: {
      auto r = one(sql::kDocument);
      if (!r) return fail(r.error());
      return RefPayload{DocumentValue{opt_str(r->value("content_text")), opt_str(r->value("content_json"))}};
    }
  }
  return fail(ErrorKind::Protocol, "unknown ref_type");
}

Result<std::map<std::string, std::vector<ReferenceRow>>> read_references(Db& db, const QString& sql, Uuid revision,
                                                                         const char* key_column) {
  auto rows = db.select(sql, {qv(revision)});
  if (!rows) return fail(rows.error());
  std::map<std::string, std::vector<ReferenceRow>> out;
  for (const auto& r : *rows)
    out[to_std(r.value(key_column))].push_back(ReferenceRow{r.value("ordinal").toInt(), opt_uuid(r.value("ref_analysis_uuid")),
                                                            opt_str(r.value("record_id")), r.value("exclude").toBool()});
  return out;
}

}  // namespace

Result<void> write_payload(Db& db, Uuid revision, Uuid subject, const RevisionPayload& payload) {
  return std::visit(Writer{db, revision, subject}, payload);
}

Result<Sha256Digest> put_script_text(Db& db, const std::string& body) {
  const Sha256Digest sha = sha256(std::string_view{body});
  Row r;
  r["sha256"] = qv(sha);
  r["body"] = qv(body);
  r["created_utc"] = qv(UtcTime::now());
  if (auto ins = db.insert_or_ignore("script_text", r); !ins) return fail(ins.error());
  return sha;
}

Result<RevisionPayload> read_payload(Db& db, Uuid revision, Kind kind) {
  const Bindings b{qv(revision)};
  switch (kind) {
    case Kind::Intercepts: {
      auto rows = db.select(sql::kIntercepts, b);
      if (!rows) return fail(rows.error());
      Intercepts out;
      for (const auto& r : *rows) {
        InterceptRow i;
        i.isotope = to_std(r.value("isotope"));
        i.detector = to_std(r.value("detector"));
        i.value = opt_double(r.value("value"));
        i.error = opt_double(r.value("error"));
        i.fit = opt_str(r.value("fit"));
        i.error_type = opt_str(r.value("error_type"));
        i.n = opt_int(r.value("n"));
        i.fn = opt_int(r.value("fn"));
        i.include_baseline_error = opt_bool(r.value("include_baseline_error"));
        i.filter_outliers_json = opt_str(r.value("filter_outliers"));
        i.user_excluded_json = opt_str(r.value("user_excluded"));
        i.outlier_excluded_json = opt_str(r.value("outlier_excluded"));
        i.reviewed = r.value("reviewed").toBool();
        i.manual = get_manual(r);
        i.extra_json = opt_str(r.value("extra"));
        out.push_back(std::move(i));
      }
      return RevisionPayload{std::move(out)};
    }
    case Kind::Baselines: {
      auto rows = db.select(sql::kBaselines, b);
      if (!rows) return fail(rows.error());
      Baselines out;
      for (const auto& r : *rows) {
        BaselineRow x;
        x.detector = to_std(r.value("detector"));
        x.value = opt_double(r.value("value"));
        x.error = opt_double(r.value("error"));
        x.fit = opt_str(r.value("fit"));
        x.error_type = opt_str(r.value("error_type"));
        x.n = opt_int(r.value("n"));
        x.fn = opt_int(r.value("fn"));
        x.filter_outliers_json = opt_str(r.value("filter_outliers"));
        x.user_excluded_json = opt_str(r.value("user_excluded"));
        x.modifier_value = opt_double(r.value("modifier_value"));
        x.modifier_error = opt_double(r.value("modifier_error"));
        x.reviewed = r.value("reviewed").toBool();
        x.manual = get_manual(r);
        x.extra_json = opt_str(r.value("extra"));
        out.push_back(std::move(x));
      }
      return RevisionPayload{std::move(out)};
    }
    case Kind::Blanks: {
      auto rows = db.select(sql::kBlanks, b);
      if (!rows) return fail(rows.error());
      auto refs = read_references(db, sql::kBlankRefs, revision, "isotope");
      if (!refs) return fail(refs.error());
      Blanks out;
      for (const auto& r : *rows) {
        BlankRow x;
        x.isotope = to_std(r.value("isotope"));
        x.value = opt_double(r.value("value"));
        x.error = opt_double(r.value("error"));
        x.fit = opt_str(r.value("fit"));
        x.error_type = opt_str(r.value("error_type"));
        x.reviewed = r.value("reviewed").toBool();
        x.manual = get_manual(r);
        x.extra_json = opt_str(r.value("extra"));
        if (auto it = refs->find(x.isotope); it != refs->end()) x.references = it->second;
        out.push_back(std::move(x));
      }
      return RevisionPayload{std::move(out)};
    }
    case Kind::IcFactors: {
      auto rows = db.select(sql::kIcFactors, b);
      if (!rows) return fail(rows.error());
      auto refs = read_references(db, sql::kIcFactorRefs, revision, "detector");
      if (!refs) return fail(refs.error());
      IcFactors out;
      for (const auto& r : *rows) {
        IcFactorRow x;
        x.detector = to_std(r.value("detector"));
        x.value = opt_double(r.value("value"));
        x.error = opt_double(r.value("error"));
        x.fit = opt_str(r.value("fit"));
        x.reviewed = r.value("reviewed").toBool();
        x.reference_detector = opt_str(r.value("reference_detector"));
        x.standard_ratio = opt_double(r.value("standard_ratio"));
        x.discrimination = r.value("discrimination").toBool();
        x.source_correction = r.value("source_correction").toBool();
        x.reference_data_json = opt_str(r.value("reference_data"));
        x.manual = get_manual(r);
        x.extra_json = opt_str(r.value("extra"));
        if (auto it = refs->find(x.detector); it != refs->end()) x.references = it->second;
        out.push_back(std::move(x));
      }
      return RevisionPayload{std::move(out)};
    }
    case Kind::Signals: {
      auto rows = db.select(sql::kSignalRefs, b);
      if (!rows) return fail(rows.error());
      SignalRefs out;
      for (const auto& r : *rows)
        out.push_back(SignalRefRow{to_std(r.value("series_kind")), to_std(r.value("series_key")),
                                   to_std(r.value("detector")), to_digest(r.value("blob_sha")),
                                   opt_int(r.value("n_points")), opt_int(r.value("start_index")),
                                   opt_int(r.value("end_index"))});
      return RevisionPayload{std::move(out)};
    }
    case Kind::Tags: {
      auto row = db.select_one(sql::kTag, b);
      if (!row) return fail(row.error());
      if (!*row) return fail(ErrorKind::Protocol, "tags revision has no tag_value row");
      return RevisionPayload{TagValue{to_std((*row)->value("name")), opt_str((*row)->value("note")),
                                      opt_str((*row)->value("subgroup"))}};
    }
    case Kind::Annotation: {
      auto row = db.select_one(sql::kAnnotation, b);
      if (!row) return fail(row.error());
      if (!*row) return fail(ErrorKind::Protocol, "annotation revision has no annotation_value row");
      return RevisionPayload{AnnotationValue{opt_str((*row)->value("comment"))}};
    }
    case Kind::RefPins: {
      auto rows = db.select(sql::kRefPins, b);
      if (!rows) return fail(rows.error());
      RefPins out;
      for (const auto& r : *rows)
        out.push_back(RefPinRow{to_uuid(r.value("ref_object_uuid")), to_uuid(r.value("ref_revision_uuid"))});
      return RevisionPayload{std::move(out)};
    }
    case Kind::Cosmogenic: {
      auto row = db.select_one(sql::kCosmogenic, b);
      if (!row) return fail(row.error());
      if (!*row) return fail(ErrorKind::Protocol, "cosmogenic revision has no cosmogenic_value row");
      return RevisionPayload{CosmogenicValue{to_std((*row)->value("doc"))}};
    }
    case Kind::Identity: {
      auto row = db.select_one(sql::kIdentity, b);
      if (!row) return fail(row.error());
      if (!*row) return fail(ErrorKind::Protocol, "identity revision has no identity_value row");
      return RevisionPayload{IdentityValue{to_uuid((*row)->value("identifier_uuid")), (*row)->value("aliquot").toInt(),
                                           (*row)->value("increment").toInt(), to_std((*row)->value("reason"))}};
    }
    case Kind::InterpretedAge: {
      auto row = db.select_one(sql::kIaValue, b);
      if (!row) return fail(row.error());
      if (!*row) return fail(ErrorKind::Protocol, "interpreted_age revision has no ia_value row");
      const Row& r = **row;
      InterpretedAgeValue v;
      v.age = opt_double(r.value("age"));
      v.age_err = opt_double(r.value("age_err"));
      v.age_kind = opt_str(r.value("age_kind"));
      v.kca = opt_double(r.value("kca"));
      v.kca_err = opt_double(r.value("kca_err"));
      v.mswd = opt_double(r.value("mswd"));
      v.nanalyses = opt_int(r.value("nanalyses"));
      v.doc_json = to_std(r.value("doc"));
      auto members = db.select(sql::kIaMembers, b);
      if (!members) return fail(members.error());
      for (const auto& m : *members)
        v.members.push_back(InterpretedAgeMember{to_uuid(m.value("analysis_uuid")), opt_str(m.value("record_id")),
                                                 opt_bool(m.value("plateau_step")), opt_str(m.value("tag"))});
      return RevisionPayload{std::move(v)};
    }
    case Kind::RefValue: {
      auto p = read_ref_payload(db, revision);
      if (!p) return fail(p.error());
      return RevisionPayload{std::move(*p)};
    }
  }
  return fail(ErrorKind::Protocol, "unknown payload kind");
}

}  // namespace pychron::persistence::detail
