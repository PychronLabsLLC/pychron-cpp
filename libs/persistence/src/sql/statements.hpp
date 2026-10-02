#pragma once

// Every SQL statement the access layer runs (DVC schema spec, P6). Inserts go
// through the TinyORM query builder; everything that needs a dialect-neutral
// compare-and-swap, RETURNING, joins or engine introspection is here.
// Placeholders are '?' on both engines.

#include <QString>
#include <QStringList>

#include "pychron/persistence/store.hpp"

namespace pychron::persistence::detail::sql {

// A timestamptz column read back as "YYYY-MM-DDTHH:MM:SS.ffffffZ". QtSql
// would hand PostgreSQL timestamps over as QDateTime, which drops microseconds.
inline QString ts(Dialect d, const QString& column) {
  if (d == Dialect::Sqlite) return column;
  return QStringLiteral(R"(to_char(%1 AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'))").arg(column);
}

inline QString placeholders(qsizetype n) {
  QStringList p;
  for (qsizetype i = 0; i < n; ++i) p << QStringLiteral("?");
  return p.join(QStringLiteral(", "));
}

// ---------------------------------------------------------------- schema

inline QString schema_version_exists(Dialect d) {
  if (d == Dialect::Sqlite)
    return QStringLiteral("SELECT count(*) AS n FROM sqlite_master WHERE type = 'table' AND name = 'schema_version'");
  return QStringLiteral(
      "SELECT count(*) AS n FROM information_schema.tables "
      "WHERE table_schema = current_schema() AND table_name = 'schema_version'");
}

inline const QString kSelectSchemaVersions =
    QStringLiteral("SELECT version, description, checksum FROM schema_version ORDER BY version");

// Transaction-scoped advisory lock; the key is arbitrary but fixed ("pychron").
inline const QString kPgMigrationLock = QStringLiteral("SELECT pg_advisory_xact_lock(7083117165926551)");

inline const QString kSqliteVersion = QStringLiteral("SELECT sqlite_version() AS v");
inline const QString kSqliteForeignKeys = QStringLiteral("PRAGMA foreign_keys");
inline const QString kSqliteForeignKeysOn = QStringLiteral("PRAGMA foreign_keys = ON");
inline const QString kSqliteWal = QStringLiteral("PRAGMA journal_mode = WAL");
inline const QString kSqliteSynchronousFull = QStringLiteral("PRAGMA synchronous = FULL");

// ---------------------------------------------------------------- change cursor (9.1)

inline const QString kNextChangeSeq =
    QStringLiteral("UPDATE change_counter SET seq = seq + 1 WHERE id = 1 RETURNING seq");
inline const QString kNotify = QStringLiteral("SELECT pg_notify('pychron_change', ?)");

inline const QString kChangesSince = QStringLiteral(
    "SELECT change_seq, %1 AS committed, changeset_uuid, client_uuid, kind FROM change_log "
    "WHERE change_seq > ? ORDER BY change_seq LIMIT ?");
inline const QString kChangeEntitiesBetween = QStringLiteral(
    "SELECT change_seq, entity_type, entity_uuid, op FROM change_entity "
    "WHERE change_seq > ? AND change_seq <= ? ORDER BY change_seq, entity_type, entity_uuid");

// ---------------------------------------------------------------- heads and CAS (5.4)

inline const QString kCasHead = QStringLiteral(
    "UPDATE head SET revision_uuid = ?, head_version = head_version + 1 "
    "WHERE subject_uuid = ? AND kind = ? AND revision_uuid = ?");
inline const QString kInsertFirstHead = QStringLiteral(
    "INSERT INTO head (subject_uuid, kind, revision_uuid, head_version) VALUES (?, ?, ?, 1) "
    "ON CONFLICT DO NOTHING");
inline const QString kSelectHead =
    QStringLiteral("SELECT revision_uuid FROM head WHERE subject_uuid = ? AND kind = ?");
inline const QString kSelectHeads = QStringLiteral(
    "SELECT subject_uuid, kind, revision_uuid, head_version FROM head WHERE subject_uuid = ? ORDER BY kind");

inline const QString kChangesetOfRevision = QStringLiteral(
    "SELECT c.uuid, c.kind, c.author_user_uuid, c.client_uuid, %1 AS created, c.message "
    "FROM revision r JOIN changeset c ON c.uuid = r.changeset_uuid WHERE r.uuid = ?");

inline const QString kHistory = QStringLiteral(
    "SELECT r.uuid, r.parent_uuid, r.subject_uuid, r.kind, c.uuid AS cs_uuid, c.kind AS cs_kind, "
    "c.author_user_uuid, c.client_uuid, %1 AS cs_created, c.message, l.change_seq "
    "FROM revision r JOIN changeset c ON c.uuid = r.changeset_uuid "
    "LEFT JOIN change_log l ON l.changeset_uuid = c.uuid "
    "WHERE r.subject_uuid = ? AND r.kind = ? ORDER BY l.change_seq, r.created_utc, r.uuid");

inline const QString kRevisionKind = QStringLiteral("SELECT kind FROM revision WHERE uuid = ?");

// ---------------------------------------------------------------- catalog

inline const QString kClientByHost = QStringLiteral("SELECT uuid FROM client WHERE hostname = ? AND role = ?");
inline const QString kUserByName = QStringLiteral("SELECT uuid FROM app_user WHERE name = ?");
inline const QString kLoadByName = QStringLiteral("SELECT uuid FROM load WHERE name = ?");
inline const QString kIdentifierByText = QStringLiteral("SELECT uuid FROM identifier WHERE identifier = ?");
inline const QString kMassSpecByName = QStringLiteral("SELECT uuid FROM mass_spectrometer WHERE name = ?");
inline const QString kExtractDeviceByName = QStringLiteral("SELECT uuid FROM extract_device WHERE name = ?");

// ---------------------------------------------------------------- ingest (8.3)

inline const QString kReceipt =
    QStringLiteral("SELECT payload_sha256, change_seq FROM ingest_receipt WHERE item_uuid = ?");
inline QString blobs_present(qsizetype n) {
  return QStringLiteral("SELECT count(*) AS n FROM signal_blob WHERE sha256 IN (%1)").arg(placeholders(n));
}
inline const QString kPendingAnalysesForBlob = QStringLiteral(
    "SELECT DISTINCT r.analysis_uuid AS analysis_uuid FROM signal_ref s "
    "JOIN revision r ON r.uuid = s.revision_uuid JOIN analysis a ON a.uuid = r.analysis_uuid "
    "WHERE s.blob_sha = ? AND a.signals_state = 'pending'");
inline const QString kMissingBlobsOfAnalysis = QStringLiteral(
    "SELECT count(*) AS n FROM signal_ref s JOIN revision r ON r.uuid = s.revision_uuid "
    "WHERE r.analysis_uuid = ? AND NOT EXISTS (SELECT 1 FROM signal_blob b WHERE b.sha256 = s.blob_sha)");
inline const QString kMarkSignalsComplete =
    QStringLiteral("UPDATE analysis SET signals_state = 'complete' WHERE uuid = ? AND signals_state = 'pending'");

// ---------------------------------------------------------------- analysis reads

inline const QString kAnalysisSummarySelect = QStringLiteral(
    "SELECT a.uuid, a.runid_text, i.identifier, a.aliquot, a.increment, a.provisional, a.analysis_type, "
    "%1 AS ts, m.name AS mass_spectrometer, a.signals_state "
    "FROM analysis a JOIN identifier i ON i.uuid = a.identifier_uuid "
    "JOIN mass_spectrometer m ON m.uuid = a.mass_spectrometer_uuid");

// ---------------------------------------------------------------- payload reads

inline const QString kIntercepts = QStringLiteral(
    "SELECT * FROM intercept_value WHERE revision_uuid = ? ORDER BY isotope");
inline const QString kBaselines =
    QStringLiteral("SELECT * FROM baseline_value WHERE revision_uuid = ? ORDER BY detector");
inline const QString kBlanks = QStringLiteral("SELECT * FROM blank_value WHERE revision_uuid = ? ORDER BY isotope");
inline const QString kBlankRefs = QStringLiteral(
    "SELECT * FROM blank_reference WHERE revision_uuid = ? ORDER BY isotope, ordinal");
inline const QString kIcFactors =
    QStringLiteral("SELECT * FROM icfactor_value WHERE revision_uuid = ? ORDER BY detector");
inline const QString kIcFactorRefs = QStringLiteral(
    "SELECT * FROM icfactor_reference WHERE revision_uuid = ? ORDER BY detector, ordinal");
inline const QString kSignalRefs = QStringLiteral(
    "SELECT * FROM signal_ref WHERE revision_uuid = ? ORDER BY series_kind, series_key");
inline const QString kTag = QStringLiteral("SELECT * FROM tag_value WHERE revision_uuid = ?");
inline const QString kAnnotation = QStringLiteral("SELECT * FROM annotation_value WHERE revision_uuid = ?");
inline const QString kRefPins =
    QStringLiteral("SELECT * FROM refpin_value WHERE revision_uuid = ? ORDER BY ref_object_uuid");
inline const QString kCosmogenic = QStringLiteral("SELECT * FROM cosmogenic_value WHERE revision_uuid = ?");

}  // namespace pychron::persistence::detail::sql
