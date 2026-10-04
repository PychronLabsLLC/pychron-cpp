#pragma once

// The words in a conflict's detail that decide how verify counts it (legacy
// ingestion spec, section 10, items 26, 29, 35 and 37). Whoever writes such a
// detail and whoever reads it use these names, so the two cannot drift.
//
// A pending conflict is a warning (it annotates a row that was imported)
// only when its kind is identity_clash and its detail carries one of the
// markers below as `true`; the late marker also needs the reason
// kReasonLateRevisionNotApplied. Every other pending conflict is blocking.
// Each marker has one producer:
//
//   kMarkerImported      the catalog adapter: a row imported without an
//                        optional link that is broken in the dump, or a
//                        sample imported under the placeholder material
//                        (false on the refusal of a row: blocking)
//   kMarkerSynthesized   the project adapter with catalog_from_repos: a
//                        catalog row made from repository contents
//   kMarkerLate          the writer: a revision not written behind a stored
//                        one, or over a head this source did not make. A
//                        revision kept back because a stored revision comes
//                        from a commit the walk no longer has is not marked:
//                        that history was rewritten, and it is blocking.

namespace pychron::ingest {

inline constexpr char kMarkerImported[] = "imported";
inline constexpr char kMarkerSynthesized[] = "synthesized";
inline constexpr char kMarkerLate[] = "late";

// The member of a detail that says why, and the values other code decides by.
inline constexpr char kDetailReason[] = "reason";
inline constexpr char kReasonLateRevisionNotApplied[] = "late_revision_not_applied";
// A spectrometer settings file that arrives after an analysis naming it was
// folded (spec 10.29). Never superseded by a later version of the file.
inline constexpr char kReasonSpectrometerFileAfterCollection[] = "spectrometer_file_after_collection";

}  // namespace pychron::ingest
