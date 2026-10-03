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

// An import batch has one change_log entry for several changesets, so it
// names them in change_entity ('changeset') instead of change_log.
inline const QString kHistory = QStringLiteral(
    "SELECT r.uuid, r.parent_uuid, r.subject_uuid, r.kind, c.uuid AS cs_uuid, c.kind AS cs_kind, "
    "c.author_user_uuid, c.client_uuid, %1 AS cs_created, c.message, "
    "COALESCE(l.change_seq, (SELECT MIN(e.change_seq) FROM change_entity e "
    "WHERE e.entity_type = 'changeset' AND e.entity_uuid = c.uuid)) AS visible_seq, "
    "u.name AS author_name, cl.hostname AS client_hostname "
    "FROM revision r JOIN changeset c ON c.uuid = r.changeset_uuid "
    "LEFT JOIN change_log l ON l.changeset_uuid = c.uuid "
    "LEFT JOIN app_user u ON u.uuid = c.author_user_uuid "
    "LEFT JOIN client cl ON cl.uuid = c.client_uuid "
    "WHERE r.subject_uuid = ? AND r.kind = ? ORDER BY visible_seq, r.created_utc, r.uuid");

inline const QString kRevisionKind = QStringLiteral("SELECT kind FROM revision WHERE uuid = ?");

// Identity revisions (5.6): the only UPDATE of analysis identity columns.
inline const QString kIdentifierText = QStringLiteral("SELECT identifier FROM identifier WHERE uuid = ?");
inline const QString kApplyIdentity = QStringLiteral(
    "UPDATE analysis SET identifier_uuid = ?, aliquot = ?, increment = ?, runid_text = ? WHERE uuid = ?");

// ---------------------------------------------------------------- catalog

inline const QString kClientByHost = QStringLiteral("SELECT uuid FROM client WHERE hostname = ? AND role = ?");
inline const QString kUserByName = QStringLiteral("SELECT uuid FROM app_user WHERE name = ?");
inline const QString kLoadByName = QStringLiteral("SELECT uuid FROM load WHERE name = ?");
inline const QString kIdentifierByText = QStringLiteral("SELECT uuid FROM identifier WHERE identifier = ?");
inline const QString kAnalysisByIdentity = QStringLiteral(
    "SELECT a.uuid FROM analysis a JOIN identifier i ON i.uuid = a.identifier_uuid "
    "WHERE i.identifier = ? AND a.aliquot = ? AND a.increment = ?");
inline const QString kIdentifierAtPosition = QStringLiteral(
    "SELECT i.identifier FROM identifier i JOIN irradiation_position p ON p.uuid = i.position_uuid "
    "JOIN level l ON l.uuid = p.level_uuid JOIN irradiation r ON r.uuid = l.irradiation_uuid "
    "WHERE r.name = ? AND l.name = ? AND p.position = ?");
inline const QString kMassSpecByName = QStringLiteral("SELECT uuid FROM mass_spectrometer WHERE name = ?");
inline const QString kExtractDeviceByName = QStringLiteral("SELECT uuid FROM extract_device WHERE name = ?");

// ---------------------------------------------------------------- ingest (8.3)

inline const QString kReceipt =
    QStringLiteral("SELECT payload_sha256, change_seq FROM ingest_receipt WHERE item_uuid = ?");
inline QString blobs_present(qsizetype n) {
  return QStringLiteral("SELECT count(*) AS n FROM signal_blob WHERE sha256 IN (%1)").arg(placeholders(n));
}
// I13: every blob referenced by an analysis's signal refs (any revision),
// peak centers, monitor checks and artifacts.
inline const QString kPendingAnalysesForBlob = QStringLiteral(
    "SELECT a.uuid AS analysis_uuid FROM analysis a WHERE a.signals_state = 'pending' AND ("
    "EXISTS (SELECT 1 FROM signal_ref s JOIN revision r ON r.uuid = s.revision_uuid "
    "        WHERE r.analysis_uuid = a.uuid AND s.blob_sha = ?) "
    "OR EXISTS (SELECT 1 FROM peak_center p WHERE p.analysis_uuid = a.uuid AND p.points_blob_sha = ?) "
    "OR EXISTS (SELECT 1 FROM monitor_check m WHERE m.analysis_uuid = a.uuid AND m.data_blob_sha = ?) "
    "OR EXISTS (SELECT 1 FROM analysis_artifact t WHERE t.analysis_uuid = a.uuid AND t.blob_sha = ?))");
inline const QString kMissingBlobsOfAnalysis = QStringLiteral(
    "SELECT count(*) AS n FROM ("
    "SELECT s.blob_sha AS sha FROM signal_ref s JOIN revision r ON r.uuid = s.revision_uuid "
    "WHERE r.analysis_uuid = ? "
    "UNION SELECT points_blob_sha FROM peak_center WHERE analysis_uuid = ? AND points_blob_sha IS NOT NULL "
    "UNION SELECT data_blob_sha FROM monitor_check WHERE analysis_uuid = ? AND data_blob_sha IS NOT NULL "
    "UNION SELECT blob_sha FROM analysis_artifact WHERE analysis_uuid = ? AND blob_sha IS NOT NULL) x "
    "WHERE NOT EXISTS (SELECT 1 FROM signal_blob b WHERE b.sha256 = x.sha)");
inline const QString kMarkSignalsComplete =
    QStringLiteral("UPDATE analysis SET signals_state = 'complete' WHERE uuid = ? AND signals_state = 'pending'");

// ---------------------------------------------------------------- groups, bookmarks (5.5)

inline const QString kHeadsInRepository = QStringLiteral(
    "SELECT h.subject_uuid, h.kind, h.revision_uuid, h.head_version FROM head h "
    "JOIN repository_member m ON m.analysis_uuid = h.subject_uuid WHERE m.repository_uuid = ?");
inline const QString kHeadsInGroup = QStringLiteral(
    "SELECT h.subject_uuid, h.kind, h.revision_uuid, h.head_version FROM head h "
    "JOIN analysis_group_member m ON m.analysis_uuid = h.subject_uuid WHERE m.group_uuid = ?");
inline const QString kBookmarkEntries = QStringLiteral(
    "SELECT subject_uuid, kind, revision_uuid FROM bookmark_entry WHERE bookmark_uuid = ? "
    "ORDER BY subject_uuid, kind");
inline const QString kCurrentHeadsOfBookmark = QStringLiteral(
    "SELECT e.subject_uuid, e.kind, h.revision_uuid FROM bookmark_entry e "
    "LEFT JOIN head h ON h.subject_uuid = e.subject_uuid AND h.kind = e.kind WHERE e.bookmark_uuid = ?");
inline const QString kCollectionRevisions = QStringLiteral(
    "SELECT r.uuid, r.kind FROM revision r JOIN analysis a ON a.ingest_changeset_uuid = r.changeset_uuid "
    "WHERE a.uuid = ? AND r.subject_uuid = a.uuid");

// ---------------------------------------------------------------- reference resolution (6.2)

inline const QString kAnalysisScope = QStringLiteral(
    "SELECT a.mass_spectrometer_uuid, i.position_uuid, p.level_uuid, l.irradiation_uuid FROM analysis a "
    "JOIN identifier i ON i.uuid = a.identifier_uuid "
    "LEFT JOIN irradiation_position p ON p.uuid = i.position_uuid "
    "LEFT JOIN level l ON l.uuid = p.level_uuid WHERE a.uuid = ?");
inline const QString kRefCandidates = QStringLiteral(
    "SELECT o.uuid, o.ref_type, o.key, h.revision_uuid FROM ref_object o "
    "JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' "
    "WHERE (o.ref_type = 'flux_position' AND o.position_uuid = ?) "
    "OR (o.ref_type IN ('level_geometry', 'level_production') AND o.level_uuid = ?) "
    "OR (o.ref_type = 'chronology' AND o.irradiation_uuid = ?) "
    "OR (o.ref_type IN ('gains', 'sensitivity') AND o.mass_spectrometer_uuid = ?)");
inline const QString kRefObjectAtHead = QStringLiteral(
    "SELECT o.uuid, o.ref_type, o.key, h.revision_uuid FROM ref_object o "
    "JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' WHERE o.uuid = ?");

// ---------------------------------------------------------------- derived cache (4.3)

inline const QString kDerivedRows = QStringLiteral(
    "SELECT name, value, error, units FROM derived_value WHERE analysis_uuid = ? AND fingerprint = ? "
    "ORDER BY name");
inline const QString kDerivedVersions =
    QStringLiteral("SELECT DISTINCT reduction_version FROM derived_value WHERE analysis_uuid = ?");
inline const QString kPruneDerived = QStringLiteral(
    "DELETE FROM derived_value WHERE analysis_uuid = ? AND reduction_version = ? AND fingerprint <> ?");

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
inline const QString kIdentity = QStringLiteral("SELECT * FROM identity_value WHERE revision_uuid = ?");
inline const QString kIaValue = QStringLiteral("SELECT * FROM ia_value WHERE revision_uuid = ?");
inline const QString kIaMembers =
    QStringLiteral("SELECT * FROM ia_member WHERE revision_uuid = ? ORDER BY analysis_uuid");

// ---------------------------------------------------------------- reference payloads

inline const QString kRefTypeOfObject = QStringLiteral("SELECT ref_type FROM ref_object WHERE uuid = ?");
inline const QString kRefTypeOfRevision = QStringLiteral(
    "SELECT o.ref_type FROM revision r JOIN ref_object o ON o.uuid = r.ref_object_uuid WHERE r.uuid = ?");
inline const QString kFlux = QStringLiteral("SELECT * FROM flux_value WHERE revision_uuid = ?");
inline const QString kFluxAnalyses =
    QStringLiteral("SELECT * FROM flux_value_analysis WHERE revision_uuid = ? ORDER BY record_id");
inline const QString kLevelZ = QStringLiteral("SELECT * FROM level_z_value WHERE revision_uuid = ?");
inline const QString kProductionMeta = QStringLiteral("SELECT * FROM production_meta WHERE revision_uuid = ?");
inline const QString kProductionValues =
    QStringLiteral("SELECT * FROM production_value WHERE revision_uuid = ? ORDER BY key");
inline const QString kLevelProduction = QStringLiteral("SELECT * FROM level_production_value WHERE revision_uuid = ?");
inline const QString kChronology = QStringLiteral(
    "SELECT ordinal, power, %1 AS start_ts, %2 AS end_ts FROM chronology_dose WHERE revision_uuid = ? "
    "ORDER BY ordinal");
inline const QString kGains = QStringLiteral("SELECT * FROM detector_gain WHERE revision_uuid = ? ORDER BY detector");
inline const QString kSensitivity = QStringLiteral(
    "SELECT sensitivity, %1 AS create_ts, extra FROM sensitivity_value WHERE revision_uuid = ?");
inline const QString kHolderMeta = QStringLiteral("SELECT * FROM holder_meta WHERE revision_uuid = ?");
inline const QString kHolderHoles =
    QStringLiteral("SELECT * FROM holder_hole WHERE revision_uuid = ? ORDER BY ordinal");
inline const QString kScriptVersion = QStringLiteral(
    "SELECT t.body FROM script_version v JOIN script_text t ON t.sha256 = v.script_sha WHERE v.revision_uuid = ?");
inline const QString kDocument = QStringLiteral("SELECT * FROM ref_document WHERE revision_uuid = ?");

// ---------------------------------------------------------------- import bookkeeping

inline const QString kImportSourceColumns = QStringLiteral(
    "uuid, kind, url_or_path, branch, head_commit_sha, progress_commit_sha, commits_total, commits_done, "
    "importer_version, lab_time_zone, %1 AS started, %2 AS finished, status");
inline const QString kImportSourceByUuid =
    QStringLiteral("SELECT %1 FROM import_source WHERE uuid = ?");
inline const QString kImportSources = QStringLiteral("SELECT %1 FROM import_source ORDER BY started_utc, uuid");

inline const QString kProvenanceFor = QStringLiteral(
    "SELECT entity_type, entity_uuid, import_source_uuid, path, commit_sha, git_blob_sha, git_author, %1 AS git_ts, "
    "detail "
    "FROM import_provenance WHERE entity_uuid = ? ORDER BY entity_type, import_source_uuid");
inline const QString kSetProvenanceDetail = QStringLiteral(
    "UPDATE import_provenance SET detail = ? WHERE entity_type = ? AND entity_uuid = ? AND import_source_uuid = ?");
inline const QString kHasProvenance = QStringLiteral(
    "SELECT 1 AS present FROM import_provenance WHERE import_source_uuid = ? AND commit_sha = ? AND path = ? LIMIT 1");
inline const QString kHasProvenanceBlob = QStringLiteral(
    "SELECT 1 AS present FROM import_provenance WHERE import_source_uuid = ? AND path = ? AND git_blob_sha = ? "
    "LIMIT 1");
inline const QString kHasConflict = QStringLiteral(
    "SELECT 1 AS present FROM import_conflict WHERE import_source_uuid = ? AND path = ? AND file_sha256 = ? LIMIT 1");
inline const QString kImportedHeadBlobSha = QStringLiteral(
    "SELECT p.git_blob_sha FROM head h JOIN import_provenance p ON p.entity_type = 'revision' "
    "AND p.entity_uuid = h.revision_uuid AND p.import_source_uuid = ? "
    "WHERE h.subject_uuid = ? AND h.kind = ?");
inline const QString kRevisionExists = QStringLiteral("SELECT 1 AS present FROM revision WHERE uuid = ?");
// %1: ", head_commit_sha = ?" and/or ", finished_utc = ?", or nothing.
inline const QString kSetImportProgress = QStringLiteral(
    "UPDATE import_source SET progress_commit_sha = ?, commits_done = ?, commits_total = ?, status = ?%1 "
    "WHERE uuid = ?");
inline const QString kResolveConflict =
    QStringLiteral("UPDATE import_conflict SET resolution = ? WHERE uuid = ? AND import_source_uuid = ?");
inline const QString kImportConflicts = QStringLiteral(
    "SELECT uuid, path, entity_uuid, conflict_kind, db_head_revision_uuid, file_sha256, detail, resolution "
    "FROM import_conflict");

}  // namespace pychron::persistence::detail::sql
