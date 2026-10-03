#pragma once

// What the import tests of the three adapters share to verify a source: run
// ingest::verify over an adapter, name what it lists, and take one row out of
// the store through the white-box connection to see that verify notices.
// `World` is each test file's own: store, client and db.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "pychron/ingest/verify.hpp"
#include "tiny/db.hpp"

namespace pychron::dvc::testing {

inline ingest::WriterConfig verify_writer_config(const std::string& zone = "America/Denver") {
  ingest::WriterConfig c;
  c.importer_version = "pychron-import/test";
  c.lab_time_zone = zone;
  return c;
}

// Every member that reaches the age function is not comparable: these tests
// are about accounting, not reduction.
inline ingest::AgeFn no_ages() {
  return [](persistence::Uuid, const ingest::AsOf&) -> Result<ingest::ParityAge> {
    return ingest::ParityAge{ingest::NotComparable{"not reduced in this test"}};
  };
}

template <class World>
ingest::VerifyReport verify_source(World& w, ingest::ISourceAdapter& adapter, const ingest::AgeFn& age_fn = no_ages()) {
  auto report = ingest::verify(*w.store, w.client, adapter, verify_writer_config(), age_fn);
  EXPECT_TRUE(report) << (report ? "" : to_string(report.error()));
  return report ? *report : ingest::VerifyReport{};
}

// "<path> @<commit>" of each unit verify could not account for, sorted.
inline std::string unit_name(const std::string& commit, const std::string& path) { return path + " @" + commit; }
inline std::vector<std::string> sorted(std::vector<std::string> names) {
  std::sort(names.begin(), names.end());
  return names;
}
inline std::vector<std::string> unaccounted(const ingest::VerifyReport& report) {
  std::vector<std::string> out;
  for (const auto& open : report.unaccounted) out.push_back(unit_name(open.unit.commit, open.unit.path));
  return sorted(std::move(out));
}
// The unaccounted unit of that name; fails the test when there is none.
inline const ingest::UnaccountedUnit& unaccounted_unit(const ingest::VerifyReport& report, const std::string& commit,
                                                       const std::string& path) {
  static const ingest::UnaccountedUnit kNone{};
  for (const auto& open : report.unaccounted)
    if (open.unit.commit == commit && open.unit.path == path) return open;
  ADD_FAILURE() << "not listed as unaccounted: " << unit_name(commit, path);
  return kNone;
}

// Deletes rows behind the store's back; returns how many went.
template <class World>
int forget(World& w, const std::string& sql, const persistence::detail::Bindings& bindings = {}) {
  auto gone = w.db->affecting(QString::fromStdString(sql), bindings);
  EXPECT_TRUE(gone) << sql << (gone ? "" : ": " + to_string(gone.error()));
  return gone ? *gone : -1;
}

// Marks every pending conflict of the store as dealt with.
template <class World>
void resolve_pending(World& w, persistence::Uuid source) {
  auto rows = w.store->import_conflicts({source, std::nullopt, std::string("pending")});
  ASSERT_TRUE(rows);
  auto uow = w.store->begin_import_batch(source, w.client);
  ASSERT_TRUE(uow);
  for (const auto& row : *rows) ASSERT_TRUE((*uow)->resolve_conflict(row.uuid, "ignored"));
  ASSERT_TRUE((*uow)->commit());
}

}  // namespace pychron::dvc::testing
