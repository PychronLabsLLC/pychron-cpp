#pragma once

// Statements of the publishing tables (data publishing spec, section 4):
// releases, embargoes, external ids. The embargo scope lookup is the one
// visibility rule's "under a live embargo" half (spec 4.5), written once here
// and in the pub_* views of migration 0007.

#include <QString>

#include "sql/statements.hpp"

namespace pychron::persistence::detail::sql {

// Every column of `release`, times as text. `where` is appended as given.
inline QString release_select(Dialect d, const QString& where) {
  return QStringLiteral(
             "SELECT r.uuid, r.name, r.title, r.description, r.version, r.supersedes_uuid, r.group_uuid, "
             "r.bookmark_uuid, r.state, r.visibility, r.licence, r.authors, r.publisher, %1 AS embargo_until, "
             "r.doi, r.doi_state, r.options, r.reduction_version, r.software_version, r.manifest_sha256, "
             "r.created_by_user_uuid, r.client_uuid, %2 AS created, %3 AS published, %4 AS withdrawn, "
             "r.withdrawn_reason FROM release r %5")
      .arg(ts(d, QStringLiteral("r.embargo_until_utc")), ts(d, QStringLiteral("r.created_utc")),
           ts(d, QStringLiteral("r.published_utc")), ts(d, QStringLiteral("r.withdrawn_utc")), where);
}

inline const QString kReleaseByUuid = QStringLiteral("WHERE r.uuid = ?");
inline const QString kReleaseByNameVersion = QStringLiteral("WHERE r.name = ? AND r.version = ?");
inline const QString kReleaseNewestOfName = QStringLiteral("WHERE r.name = ? ORDER BY r.version DESC LIMIT 1");
inline const QString kReleaseOrder = QStringLiteral("ORDER BY r.name, r.version");

inline const QString kReleaseSamples =
    QStringLiteral("SELECT sample_uuid FROM release_sample WHERE release_uuid = ? ORDER BY sample_uuid");
inline const QString kReleaseInterpretedAges = QStringLiteral(
    "SELECT ia_uuid, revision_uuid FROM release_interpreted_age WHERE release_uuid = ? ORDER BY ia_uuid");
inline const QString kDeleteReleaseSamples = QStringLiteral("DELETE FROM release_sample WHERE release_uuid = ?");
inline const QString kDeleteReleaseInterpretedAges =
    QStringLiteral("DELETE FROM release_interpreted_age WHERE release_uuid = ?");
inline const QString kDeleteRelease = QStringLiteral("DELETE FROM release WHERE uuid = ?");

// ---------------------------------------------------------------- embargoes

// A live embargo: not lifted, and either open-ended or not yet at its date.
inline const QString kLiveEmbargo =
    QStringLiteral("e.lifted_utc IS NULL AND (e.until_utc IS NULL OR e.until_utc > ?)");

// The samples among `n` given that are under a live sample or project
// embargo: one statement, each scope found by embargo_scope_ix. Bindings:
// the n sample uuids, then the time twice.
inline QString embargoed_samples(qsizetype n) {
  return QStringLiteral(
             "SELECT s.uuid FROM sample s WHERE s.uuid IN (%1) AND EXISTS ("
             "SELECT 1 FROM embargo e WHERE %2 AND e.scope_type = 'sample' AND e.scope_uuid = s.uuid "
             "UNION ALL "
             "SELECT 1 FROM embargo e WHERE %2 AND e.scope_type = 'project' AND e.scope_uuid = s.project_uuid) "
             "ORDER BY s.uuid")
      .arg(placeholders(n), kLiveEmbargo);
}

// The analyses among `n` given that are under a live analysis embargo.
// Bindings: the n analysis uuids, then the time.
inline QString embargoed_analyses_direct(qsizetype n) {
  return QStringLiteral(
             "SELECT e.scope_uuid AS uuid FROM embargo e WHERE e.scope_type = 'analysis' AND e.scope_uuid IN (%1) "
             "AND %2 ORDER BY e.scope_uuid")
      .arg(placeholders(n), kLiveEmbargo);
}

// Each analysis with its sample: the identifier's own, else its irradiation
// position's (as browse.cpp finds them). Bindings: the n analysis uuids.
inline QString analysis_samples(qsizetype n) {
  return QStringLiteral(
             "SELECT a.uuid, i.sample_uuid AS own, ip.sample_uuid AS positioned FROM analysis a "
             "JOIN identifier i ON i.uuid = a.identifier_uuid "
             "LEFT JOIN irradiation_position ip ON ip.uuid = i.position_uuid WHERE a.uuid IN (%1)")
      .arg(placeholders(n));
}

inline QString embargoes_select(Dialect d, bool live_only) {
  return QStringLiteral(
             "SELECT e.uuid, e.scope_type, e.scope_uuid, %1 AS until_t, e.reason, e.set_by_user_uuid, "
             "%2 AS created, %3 AS lifted, e.lifted_by_user_uuid FROM embargo e %4 ORDER BY e.created_utc, e.uuid")
      .arg(ts(d, QStringLiteral("e.until_utc")), ts(d, QStringLiteral("e.created_utc")),
           ts(d, QStringLiteral("e.lifted_utc")), live_only ? QStringLiteral("WHERE ") + kLiveEmbargo : QString());
}

inline const QString kEmbargoLifted = QStringLiteral("SELECT lifted_utc FROM embargo WHERE uuid = ?");
inline const QString kLiftEmbargo =
    QStringLiteral("UPDATE embargo SET lifted_utc = ?, lifted_by_user_uuid = ? WHERE uuid = ? AND lifted_utc IS NULL");

// ---------------------------------------------------------------- external ids

inline QString external_ids_select(Dialect d) {
  return QStringLiteral(
             "SELECT x.uuid, x.entity_type, x.entity_uuid, x.scheme, x.value, x.registrar, x.state, "
             "x.request_sha256, x.response, %1 AS registered FROM external_id x "
             "WHERE x.entity_type = ? AND x.entity_uuid = ? ORDER BY x.registered_utc, x.uuid")
      .arg(ts(d, QStringLiteral("x.registered_utc")));
}

}  // namespace pychron::persistence::detail::sql
