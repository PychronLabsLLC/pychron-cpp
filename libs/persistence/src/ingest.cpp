// Idempotent ingest of outbox items (DVC schema spec, sections 5.3, 7.3, 8.3).

#include <set>

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {
namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

Result<std::optional<Uuid>> lookup(Db& db, const QString& sql, const std::string& key) {
  auto row = db.select_one(sql, {qv(key)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<Uuid>{};
  return std::optional<Uuid>{to_uuid((*row)->value("uuid"))};
}

Result<Uuid> require(Db& db, const QString& sql, const std::string& key, const char* what) {
  auto id = lookup(db, sql, key);
  if (!id) return fail(id.error());
  if (!*id) return fail(ErrorKind::Protocol, std::string("unknown ") + what + " '" + key + "'");
  return **id;
}

Result<Uuid> ensure_load(Db& db, const std::string& name, std::vector<ChangeEntityRow>& created) {
  auto id = lookup(db, sql::kLoadByName, name);
  if (!id) return fail(id.error());
  if (*id) return **id;
  const Uuid uuid = Uuid::v7();
  Row r;
  r["uuid"] = qv(uuid);
  r["name"] = qv(name);
  r["archived"] = false;
  r["created_utc"] = qv(UtcTime::now());
  if (auto ins = db.insert("load", r); !ins) return fail(ins.error());
  created.push_back(ChangeEntityRow{QStringLiteral("load"), uuid, QStringLiteral("insert"), json_created({{"name", name}})});
  return uuid;
}

Result<std::optional<IngestAck>> check_receipt(Db& db, const IngestItem& item) {
  auto receipt = db.select_one(sql::kReceipt, {qv(item.item)});
  if (!receipt) return fail(receipt.error());
  if (!*receipt) return std::optional<IngestAck>{};
  if (to_digest((*receipt)->value("payload_sha256")) != item.payload_sha256)
    return fail(ErrorKind::Protocol, "IdempotencyMismatch: item " + item.item.str() +
                                         " was ingested with a different payload");
  return std::optional<IngestAck>{IngestAck{(*receipt)->value("change_seq").toLongLong(), true}};
}

Result<IngestAck> ingest_analysis(Db& db, const IngestItem& item, const AnalysisIngest& a) {
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  auto dup = check_receipt(db, item);
  if (!dup) return fail(dup.error());
  if (*dup) return **dup;

  std::vector<ChangeEntityRow> entities;
  auto identifier = require(db, sql::kIdentifierByText, a.identifier, "identifier");
  if (!identifier) return fail(identifier.error());
  auto ms = require(db, sql::kMassSpecByName, a.mass_spectrometer, "mass spectrometer");
  if (!ms) return fail(ms.error());
  std::optional<Uuid> device;
  if (a.extract_device) {
    auto d = require(db, sql::kExtractDeviceByName, *a.extract_device, "extract device");
    if (!d) return fail(d.error());
    device = *d;
  }
  auto analyst = ensure_user_row(db, a.analyst, entities);
  if (!analyst) return fail(analyst.error());
  std::optional<Uuid> load;
  if (a.load_name) {
    auto l = ensure_load(db, *a.load_name, entities);
    if (!l) return fail(l.error());
    load = *l;
  }

  // signals_state: complete only if every referenced blob is already present (I13).
  std::set<Sha256Digest> shas;
  for (const auto& s : a.roots.signal_refs) shas.insert(s.blob_sha);
  bool complete = true;
  if (!shas.empty()) {
    Bindings b;
    for (const auto& s : shas) b << qv(s);
    auto present = db.select_one(sql::blobs_present(b.size()), b);
    if (!present) return fail(present.error());
    complete = *present && (*present)->value("n").toLongLong() == static_cast<qlonglong>(shas.size());
  }

  const std::string runid = make_runid(a.identifier, a.aliquot, a.increment);
  const ChangesetInfo cs{a.changeset, ChangesetKind::Collection, *analyst, item.client, a.created,
                         "<COLLECTION> " + runid};
  if (auto r = insert_changeset(db, cs); !r) return fail(r.error());

  Row an;
  an["uuid"] = qv(a.analysis);
  an["identifier_uuid"] = qv(*identifier);
  an["aliquot"] = a.aliquot;
  an["increment"] = a.increment;
  an["provisional"] = false;
  an["runid_text"] = qv(runid);
  an["analysis_type"] = qv(a.analysis_type);
  an["experiment_type"] = qv(a.experiment_type);
  an["timestamp_utc"] = qv(a.timestamp);
  an["time_zero_utc"] = qv(a.time_zero);
  an["mass_spectrometer_uuid"] = qv(*ms);
  an["extract_device_uuid"] = qv(device);
  const auto& x = a.extraction;
  an["extract_value"] = qv(x.extract_value);
  an["extract_units"] = qv(x.extract_units);
  an["extract_duration"] = qv(x.extract_duration);
  an["cleanup_duration"] = qv(x.cleanup_duration);
  an["pre_cleanup"] = qv(x.pre_cleanup);
  an["post_cleanup"] = qv(x.post_cleanup);
  an["cryo_temperature"] = qv(x.cryo_temperature);
  an["weight"] = qv(x.weight);
  an["beam_diameter"] = qv(x.beam_diameter);
  an["pattern"] = qv(x.pattern);
  an["ramp_duration"] = qv(x.ramp_duration);
  an["ramp_rate"] = qv(x.ramp_rate);
  an["light_value"] = qv(x.light_value);
  an["tray"] = qv(x.tray);
  an["load_uuid"] = qv(load);
  an["load_holder"] = qv(a.load_holder);
  an["run_index"] = qv(a.run_index);
  an["laboratory"] = qv(a.laboratory);
  an["instrument_name"] = qv(a.instrument_name);
  an["analyst_user_uuid"] = qv(*analyst);
  an["acquisition_client_uuid"] = qv(item.client);
  an["record_sha256"] = qv(a.record_sha256);
  an["record_schema_version"] = a.record_schema_version;
  an["signals_state"] = complete ? QStringLiteral("complete") : QStringLiteral("pending");
  an["ingest_changeset_uuid"] = qv(a.changeset);
  an["created_utc"] = qv(UtcTime::now());
  if (auto r = db.insert("analysis", an); !r) return fail(r.error());

  QList<QVariantMap> isotopes;
  for (const auto& i : a.isotopes) {
    Row r;
    r["analysis_uuid"] = qv(a.analysis);
    r["isotope"] = qv(i.isotope);
    r["detector"] = qv(i.detector);
    r["units"] = qv(i.units);
    r["detector_serial"] = qv(i.detector_serial);
    r["classification"] = qv(i.classification);
    r["classification_probability"] = qv(i.classification_probability);
    isotopes.push_back(std::move(r));
  }
  if (auto r = db.insert_many("analysis_isotope", isotopes); !r) return fail(r.error());
  QList<QVariantMap> detectors;
  for (const auto& d : a.detectors) {
    Row r;
    r["analysis_uuid"] = qv(a.analysis);
    r["detector"] = qv(d.detector);
    r["deflection"] = qv(d.deflection);
    r["gain_used"] = qv(d.gain_used);
    detectors.push_back(std::move(r));
  }
  if (auto r = db.insert_many("analysis_detector", detectors); !r) return fail(r.error());

  const auto& roots = a.roots;
  const std::pair<Kind, std::pair<Uuid, RevisionPayload>> revisions[] = {
      {Kind::Signals, {roots.signals, roots.signal_refs}},
      {Kind::Intercepts, {roots.intercepts, roots.intercepts_rows}},
      {Kind::Baselines, {roots.baselines, roots.baselines_rows}},
      {Kind::Blanks, {roots.blanks, roots.blanks_rows}},
      {Kind::IcFactors, {roots.icfactors, roots.icfactors_rows}},
      {Kind::Tags, {roots.tags, roots.tag}},
  };
  for (const auto& [kind, rev] : revisions) {
    if (auto r = insert_revision(db, rev.first, a.changeset, a.analysis, kind, std::nullopt, a.created); !r)
      return fail(r.error());
    if (auto r = write_payload(db, rev.first, rev.second); !r) return fail(r.error());
    Row head;
    head["subject_uuid"] = qv(a.analysis);
    head["kind"] = qstr(to_string(kind));
    head["revision_uuid"] = qv(rev.first);
    head["head_version"] = 1;
    if (auto r = db.insert("head", head); !r) return fail(r.error());
    if (auto r = insert_head_move(db, a.changeset, a.analysis, kind, std::nullopt, rev.first, MoveReason::Ingest); !r)
      return fail(r.error());
  }

  entities.push_back(ChangeEntityRow{QStringLiteral("analysis"), a.analysis, QStringLiteral("insert"), std::nullopt});
  auto seq = take_change(db, QStringLiteral("ingest"), a.changeset, item.client, entities);
  if (!seq) return fail(seq.error());
  Row receipt;
  receipt["item_uuid"] = qv(item.item);
  receipt["client_uuid"] = qv(item.client);
  receipt["kind"] = QStringLiteral("analysis");
  receipt["payload_sha256"] = qv(item.payload_sha256);
  receipt["change_seq"] = static_cast<qlonglong>(*seq);
  if (auto r = db.insert("ingest_receipt", receipt); !r) return fail(r.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return IngestAck{*seq, false};
}

Result<IngestAck> ingest_blob(Db& db, const IngestItem& item, const BlobIngest& blob) {
  if (blob.codec.empty()) return fail(ErrorKind::Protocol, "blob ingest: empty codec");
  const Sha256Digest sha = blob_sha256(blob.codec, blob.bytes);

  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  Row r;
  r["sha256"] = qv(sha);
  r["codec"] = qv(blob.codec);
  r["byte_len"] = static_cast<qlonglong>(blob.bytes.size());
  r["n_points"] = qv(blob.n_points);
  r["bytes"] = qv(blob.bytes);
  r["created_utc"] = qv(UtcTime::now());
  auto inserted = db.insert_or_ignore("signal_blob", r);
  if (!inserted) return fail(inserted.error());
  if (*inserted == 0) return IngestAck{std::nullopt, true};  // content address already present

  auto pending = db.select(sql::kPendingAnalysesForBlob, {qv(sha)});
  if (!pending) return fail(pending.error());
  std::vector<ChangeEntityRow> completed;
  for (const auto& row : *pending) {
    const Uuid analysis = to_uuid(row.value("analysis_uuid"));
    auto missing = db.select_one(sql::kMissingBlobsOfAnalysis, {qv(analysis)});
    if (!missing) return fail(missing.error());
    if (*missing && (*missing)->value("n").toLongLong() == 0) {
      if (auto u = db.affecting(sql::kMarkSignalsComplete, {qv(analysis)}); !u) return fail(u.error());
      completed.push_back(ChangeEntityRow{QStringLiteral("analysis"), analysis, QStringLiteral("upsert"), std::nullopt});
    }
  }
  std::optional<ChangeSeq> seq;
  if (!completed.empty()) {
    auto s = take_change(db, QStringLiteral("blob_complete"), std::nullopt, item.client, completed);
    if (!s) return fail(s.error());
    seq = *s;
  }
  if (auto c = tx.commit(); !c) return fail(c.error());
  return IngestAck{seq, false};
}

}  // namespace

Result<Uuid> ensure_user_row(Db& db, const std::string& name, std::vector<ChangeEntityRow>& created) {
  auto id = lookup(db, sql::kUserByName, name);
  if (!id) return fail(id.error());
  if (*id) return **id;
  const Uuid uuid = Uuid::v7();
  Row r;
  r["uuid"] = qv(uuid);
  r["name"] = qv(name);
  r["created_utc"] = qv(UtcTime::now());
  if (auto ins = db.insert("app_user", r); !ins) return fail(ins.error());
  created.push_back(
      ChangeEntityRow{QStringLiteral("app_user"), uuid, QStringLiteral("insert"), json_created({{"name", name}})});
  return uuid;
}

Result<IngestAck> ingest_item(Db& db, const IngestItem& item) {
  if (const auto* a = std::get_if<AnalysisIngest>(&item.body)) return ingest_analysis(db, item, *a);
  return ingest_blob(db, item, std::get<BlobIngest>(item.body));
}

}  // namespace pychron::persistence::detail
