#pragma once

// SQL of the entry reads, the catalog edit batch and identifier allocation
// (entry spec, section 5). Placeholders are '?' on both engines.

#include <QString>

#include "pychron/persistence/store.hpp"

namespace pychron::persistence::detail::sql {

inline const QString kPrincipalInvestigators = QStringLiteral(
    "SELECT uuid, last_name, first_initial, display_name, affiliation, email FROM principal_investigator "
    "ORDER BY last_name, first_initial, uuid");

inline const QString kProjects = QStringLiteral(
    "SELECT p.uuid, p.name, p.pi_uuid, pi.display_name AS pi_name, p.checkin_date, p.comment, p.lab_contact, "
    "p.institution, (SELECT count(*) FROM sample s WHERE s.project_uuid = p.uuid) AS n_samples "
    "FROM project p LEFT JOIN principal_investigator pi ON pi.uuid = p.pi_uuid");

inline const QString kMaterials = QStringLiteral(
    "SELECT m.uuid, m.name, m.grainsize, (SELECT count(*) FROM sample s WHERE s.material_uuid = m.uuid) AS n_samples "
    "FROM material m ORDER BY m.name, m.grainsize, m.uuid");

// %1: updated_utc as text. Filters are appended as " AND ...".
inline const QString kSamples = QStringLiteral(
    "SELECT s.uuid, s.name, s.project_uuid, s.material_uuid, p.pi_uuid, p.name AS project_name, "
    "pi.display_name AS pi_name, m.name AS material_name, m.grainsize, s.note, s.igsn, s.lat, s.lon, s.elevation, "
    "s.storage_location, s.location, s.unit, s.lithology, s.lithology_class, s.lithology_type, s.lithology_group, "
    "s.approximate_age, %1 AS updated, "
    "(SELECT count(*) FROM irradiation_position ip WHERE ip.sample_uuid = s.uuid) AS n_positions, "
    "(SELECT count(*) FROM analysis a JOIN identifier i ON i.uuid = a.identifier_uuid "
    " LEFT JOIN irradiation_position ip ON ip.uuid = i.position_uuid "
    " WHERE ip.sample_uuid = s.uuid OR i.sample_uuid = s.uuid) AS n_analyses "
    "FROM sample s JOIN project p ON p.uuid = s.project_uuid JOIN material m ON m.uuid = s.material_uuid "
    "LEFT JOIN principal_investigator pi ON pi.uuid = p.pi_uuid WHERE 1 = 1");

// %1: created_utc as text.
inline const QString kIrradiations = QStringLiteral(
    "SELECT i.uuid, i.name, i.kind, %1 AS created, "
    "(SELECT count(*) FROM level l WHERE l.irradiation_uuid = i.uuid) AS n_levels, "
    "(SELECT count(*) FROM irradiation_position ip JOIN level l ON l.uuid = ip.level_uuid "
    " WHERE l.irradiation_uuid = i.uuid) AS n_positions, "
    "(SELECT count(*) FROM irradiation_position ip JOIN level l ON l.uuid = ip.level_uuid "
    " JOIN identifier d ON d.position_uuid = ip.uuid "
    " WHERE l.irradiation_uuid = i.uuid AND EXISTS (SELECT 1 FROM analysis a WHERE a.identifier_uuid = d.uuid)) "
    " AS n_analyzed, "
    "(SELECT count(*) FROM ref_object o JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' "
    " WHERE o.ref_type = 'chronology' AND o.irradiation_uuid = i.uuid) AS n_chronology "
    "FROM irradiation i ORDER BY i.created_utc DESC, i.name DESC, i.uuid");

inline const QString kLevelSelect = QStringLiteral(
    "SELECT l.uuid, l.irradiation_uuid, l.name, l.holder_ref_uuid, o.key AS holder_name, l.note, "
    "i.name AS irradiation_name, i.kind AS irradiation_kind FROM level l "
    "JOIN irradiation i ON i.uuid = l.irradiation_uuid "
    "LEFT JOIN ref_object o ON o.uuid = l.holder_ref_uuid");

inline const QString kSheetPositions = QStringLiteral(
    "SELECT ip.uuid, ip.position, ip.sample_uuid, s.name AS sample_name, p.name AS project_name, "
    "pi.display_name AS pi_name, m.name AS material_name, m.grainsize, ip.weight, ip.packet, ip.note, "
    "d.uuid AS identifier_uuid, d.identifier, "
    "(SELECT count(*) FROM analysis a WHERE a.identifier_uuid = d.uuid) AS n_analyses, "
    "(SELECT count(*) FROM load_position lp WHERE lp.identifier_uuid = d.uuid) AS n_loads, "
    "(SELECT f.j FROM ref_object o JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' "
    " JOIN flux_value f ON f.revision_uuid = h.revision_uuid "
    " WHERE o.ref_type = 'flux_position' AND o.position_uuid = ip.uuid) AS j, "
    "(SELECT f.j_err FROM ref_object o JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' "
    " JOIN flux_value f ON f.revision_uuid = h.revision_uuid "
    " WHERE o.ref_type = 'flux_position' AND o.position_uuid = ip.uuid) AS j_err "
    "FROM irradiation_position ip LEFT JOIN sample s ON s.uuid = ip.sample_uuid "
    "LEFT JOIN project p ON p.uuid = s.project_uuid LEFT JOIN material m ON m.uuid = s.material_uuid "
    "LEFT JOIN principal_investigator pi ON pi.uuid = p.pi_uuid "
    "LEFT JOIN identifier d ON d.position_uuid = ip.uuid "
    "WHERE ip.level_uuid = ? ORDER BY ip.position, ip.uuid");

inline const QString kLevelRefHeads = QStringLiteral(
    "SELECT o.uuid, o.ref_type, h.revision_uuid FROM ref_object o "
    "LEFT JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' "
    "WHERE o.level_uuid = ? AND o.ref_type IN ('level_geometry', 'level_production') ORDER BY o.ref_type, o.uuid");

inline const QString kIdentifierCounter = QStringLiteral("SELECT last_value FROM identifier_counter WHERE scope = ?");

inline QString max_numeric_identifier(Dialect d) {
  if (d == Dialect::PostgreSql)
    return QStringLiteral(
        "SELECT max(identifier::bigint) AS n FROM identifier WHERE identifier ~ '^[1-9][0-9]{0,17}$'");
  return QStringLiteral(
      "SELECT max(CAST(identifier AS INTEGER)) AS n FROM identifier "
      "WHERE identifier <> '' AND identifier NOT GLOB '*[^0-9]*' AND identifier NOT LIKE '0%' "
      "AND length(identifier) <= 18");
}

// ---------------------------------------------------------------- rules (5.2)

inline const QString kIdentifierUse = QStringLiteral(
    "SELECT (SELECT count(*) FROM analysis WHERE identifier_uuid = ?) AS n_analyses, "
    "(SELECT count(*) FROM load_position WHERE identifier_uuid = ?) AS n_loads, "
    "(SELECT count(*) FROM aliquot_lease WHERE identifier_uuid = ?) AS n_leases");
inline const QString kPositionIdentifier =
    QStringLiteral("SELECT uuid, identifier FROM identifier WHERE position_uuid = ?");
inline const QString kAnalysesInIrradiation = QStringLiteral(
    "SELECT count(*) AS n FROM analysis a JOIN identifier d ON d.uuid = a.identifier_uuid "
    "JOIN irradiation_position ip ON ip.uuid = d.position_uuid JOIN level l ON l.uuid = ip.level_uuid "
    "WHERE l.irradiation_uuid = ?");
inline const QString kAnalysesInLevel = QStringLiteral(
    "SELECT count(*) AS n FROM analysis a JOIN identifier d ON d.uuid = a.identifier_uuid "
    "JOIN irradiation_position ip ON ip.uuid = d.position_uuid WHERE ip.level_uuid = ?");

// Rewrites the "<old>" / "<old>/..." prefix of the keys of the references
// scoped to a package (%1 = irradiation_uuid) or a level (%1 = level_uuid).
inline const QString kRewriteRefKeys = QStringLiteral(
    "UPDATE ref_object SET key = ? || substr(key, ?) WHERE %1 = ? AND (key = ? OR substr(key, 1, ?) = ?)");

inline const QString kLockCounter =
    QStringLiteral("SELECT last_value FROM identifier_counter WHERE scope = ? FOR UPDATE");
inline const QString kSeedCounter = QStringLiteral(
    "INSERT INTO identifier_counter (scope, last_value) VALUES (?, ?) ON CONFLICT (scope) DO NOTHING");
inline const QString kSetCounter = QStringLiteral("UPDATE identifier_counter SET last_value = ? WHERE scope = ?");
inline const QString kIdentifierByTextAny = QStringLiteral("SELECT uuid FROM identifier WHERE identifier = ?");

}  // namespace pychron::persistence::detail::sql
