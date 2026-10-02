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

struct Writer {
  Db& db;
  Uuid revision;

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
};

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

Result<void> write_payload(Db& db, Uuid revision, const RevisionPayload& payload) {
  return std::visit(Writer{db, revision}, payload);
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
    case Kind::Identity:
    case Kind::InterpretedAge:
    case Kind::RefValue:
      break;
  }
  return fail(ErrorKind::Protocol, "payload of kind '" + std::string(to_string(kind)) + "' is not supported yet");
}

}  // namespace pychron::persistence::detail
