#pragma once

// SQL for browsing (data browsing and visualization design, section 9.2).
// browse.cpp composes these fragments; no SQL text lives outside src/sql/.

#include <QString>

#include "sql/statements.hpp"

namespace pychron::persistence::detail::sql {

// Every browse column an analysis row needs. %1: the timestamp expression.
inline const QString kBrowseSelect = QStringLiteral(
    "SELECT a.uuid, a.runid_text, i.identifier, a.aliquot, a.increment, a.provisional, a.analysis_type, "
    "%1 AS ts, m.name AS mass_spectrometer, a.signals_state, "
    "s.name AS sample, p.name AS project, mt.name AS material, pi.display_name AS principal_investigator, "
    "ed.name AS extract_device, ld.name AS load_name, ir.name AS irradiation, lv.name AS level, "
    "ip.position AS position, a.extract_value, a.extract_units, tv.name AS tag, "
    "(SELECT MIN(r.name) FROM repository_member rm JOIN repository r ON r.uuid = rm.repository_uuid "
    "WHERE rm.analysis_uuid = a.uuid) AS repository ");

inline const QString kBrowseFrom = QStringLiteral(
    "FROM analysis a "
    "JOIN identifier i ON i.uuid = a.identifier_uuid "
    "JOIN mass_spectrometer m ON m.uuid = a.mass_spectrometer_uuid "
    "LEFT JOIN extract_device ed ON ed.uuid = a.extract_device_uuid "
    "LEFT JOIN load ld ON ld.uuid = a.load_uuid "
    "LEFT JOIN irradiation_position ip ON ip.uuid = i.position_uuid "
    "LEFT JOIN level lv ON lv.uuid = ip.level_uuid "
    "LEFT JOIN irradiation ir ON ir.uuid = lv.irradiation_uuid "
    "LEFT JOIN sample s ON s.uuid = COALESCE(i.sample_uuid, ip.sample_uuid) "
    "LEFT JOIN project p ON p.uuid = s.project_uuid "
    "LEFT JOIN principal_investigator pi ON pi.uuid = p.pi_uuid "
    "LEFT JOIN material mt ON mt.uuid = s.material_uuid "
    "LEFT JOIN head th ON th.subject_uuid = a.uuid AND th.kind = 'tags' "
    "LEFT JOIN tag_value tv ON tv.revision_uuid = th.revision_uuid ");

// Columns a list filter or facet applies to. Repository is filtered through
// kBrowseRepositoryIn instead.
inline const QString kColAnalysisType = QStringLiteral("a.analysis_type");
inline const QString kColMassSpectrometer = QStringLiteral("m.name");
inline const QString kColExtractDevice = QStringLiteral("ed.name");
inline const QString kColProject = QStringLiteral("p.name");
inline const QString kColPrincipalInvestigator = QStringLiteral("pi.display_name");
inline const QString kColSample = QStringLiteral("s.name");
inline const QString kColMaterial = QStringLiteral("mt.name");
inline const QString kColIdentifier = QStringLiteral("i.identifier");
inline const QString kColIrradiation = QStringLiteral("ir.name");
inline const QString kColLevel = QStringLiteral("lv.name");
inline const QString kColLoad = QStringLiteral("ld.name");

// %1: column, %2: placeholders.
inline const QString kBrowseIn = QStringLiteral("%1 IN (%2)");
inline const QString kBrowseRepositoryIn = QStringLiteral(
    "EXISTS (SELECT 1 FROM repository_member rm JOIN repository r ON r.uuid = rm.repository_uuid "
    "WHERE rm.analysis_uuid = a.uuid AND r.name IN (%1))");
// Tags not excluded (an analysis without a tag head passes). %1: placeholders.
inline const QString kBrowseTagNotIn = QStringLiteral("(tv.name IS NULL OR tv.name NOT IN (%1))");
// Bindings: three lower-case prefixes ending in %, escaped with backslash.
inline const QString kBrowseText = QStringLiteral(
    "(LOWER(a.runid_text) LIKE ? ESCAPE '\\' OR LOWER(i.identifier) LIKE ? ESCAPE '\\' "
    "OR LOWER(s.name) LIKE ? ESCAPE '\\')");
inline const QString kBrowseSince = QStringLiteral("a.timestamp_utc >= ?");
inline const QString kBrowseTo = QStringLiteral("a.timestamp_utc <= ?");
// Keyset: strictly older than (ts, uuid). Bindings: ts, ts, uuid.
inline const QString kBrowseBefore =
    QStringLiteral("(a.timestamp_utc < ? OR (a.timestamp_utc = ? AND a.uuid < ?))");
inline const QString kBrowseOrder = QStringLiteral(" ORDER BY a.timestamp_utc DESC, a.uuid DESC LIMIT ?");
inline const QString kBrowseCount = QStringLiteral("SELECT COUNT(*) AS n ");
// %1: column. Distinct non-null values, sorted.
inline const QString kFacetSelect = QStringLiteral("SELECT DISTINCT %1 AS v ");
inline const QString kFacetTail = QStringLiteral(" ORDER BY 1");
inline const QString kFacetNotNull = QStringLiteral("%1 IS NOT NULL AND %1 <> ''");
inline const QString kRepositoryFacet = QStringLiteral(
    "SELECT DISTINCT r.name AS v FROM repository r JOIN repository_member rm ON rm.repository_uuid = r.uuid "
    "WHERE rm.analysis_uuid IN (SELECT a.uuid %1 %2) ORDER BY 1");
// %1: timestamp expression.
inline const QString kNewestAnalysis =
    QStringLiteral("SELECT %1 AS ts FROM analysis ORDER BY timestamp_utc DESC LIMIT 1");

// ---------------------------------------------------------------- detail

inline const QString kAnalysisDetail = QStringLiteral(
    "SELECT a.extract_value, a.extract_units, a.extract_duration, a.cleanup_duration, a.pre_cleanup, "
    "a.post_cleanup, a.cryo_temperature, a.weight, a.beam_diameter, a.pattern, a.ramp_duration, a.ramp_rate, "
    "a.light_value, a.tray, u.name AS analyst, am.environmental "
    "FROM analysis a LEFT JOIN app_user u ON u.uuid = a.analyst_user_uuid "
    "LEFT JOIN analysis_meta am ON am.analysis_uuid = a.uuid WHERE a.uuid = ?");
inline const QString kAnalysisIsotopes = QStringLiteral(
    "SELECT isotope, detector, units, detector_serial, classification, classification_probability "
    "FROM analysis_isotope WHERE analysis_uuid = ? ORDER BY isotope DESC");
inline const QString kAnalysisDetectors =
    QStringLiteral("SELECT detector, deflection, gain_used FROM analysis_detector WHERE analysis_uuid = ? ORDER BY detector");
inline const QString kAnalysisPeakCenters = QStringLiteral(
    "SELECT detector, reference_detector, reference_isotope, interpolation, low_dac, center_dac, high_dac, "
    "low_signal, center_signal, high_signal, resolution, low_resolving_power, high_resolving_power, points_blob_sha "
    "FROM peak_center WHERE analysis_uuid = ? ORDER BY detector");
inline const QString kLatestChangeSeq = QStringLiteral("SELECT COALESCE(MAX(change_seq), 0) AS n FROM change_log");
inline const QString kBlobByShaFull =
    QStringLiteral("SELECT codec, n_points, bytes FROM signal_blob WHERE sha256 = ?");

}  // namespace pychron::persistence::detail::sql
