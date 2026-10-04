#pragma once

// What the import tests of the three adapters share to verify a source: run
// ingest::verify over an adapter, name what it lists, and take one row out of
// the store through the white-box connection to see that verify notices.
// `World` is each test file's own: store, client and db.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
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
inline std::vector<persistence::Uuid> sorted_ids(std::vector<persistence::Uuid> ids) {
  std::sort(ids.begin(), ids.end());
  return ids;
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

// Where two snapshots first differ, for a readable failure.
inline std::string rows_difference(const std::vector<std::string>& got, const std::vector<std::string>& want) {
  for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
    const std::string a = i < got.size() ? got[i] : "(nothing)";
    const std::string b = i < want.size() ? want[i] : "(nothing)";
    if (a != b) return "line " + std::to_string(i) + "\n  got:  " + a.substr(0, 600) + "\n  want: " + b.substr(0, 600);
  }
  return {};
}

// One history, one result (spec 10.16) for the history a repository holds
// now: imported in the largest batches (a batch still ends where the adapter
// must end one), in small batches, stopped after every batch and resumed, and
// each of those replayed at other batch sizes, every world ends with the same
// rows. RunStats::conflicts of a whole run is what it leaves pending.
//
//   fresh()                                     a new, empty world (a unique_ptr)
//   import(world, batch_commits, max_batches, replay)   one run -> Result<RunStats>
//   snapshot(world)                             its rows -> std::vector<std::string>
//   check(world, what)                          what every world must hold
template <class Fresh, class Import, class Snapshot, class Check>
void same_at_every_cut(const Fresh& fresh, const Import& import, const Snapshot& snapshot, const Check& check) {
  auto reference = fresh();
  auto whole = import(*reference, 500, std::optional<int>{}, false);
  ASSERT_TRUE(whole) << to_string(whole.error());
  const std::vector<std::string> want = snapshot(*reference);
  ASSERT_FALSE(want.empty());
  check(*reference, "largest batches");
  int pending = 0;
  {
    auto rows = reference->store->import_conflicts({std::nullopt, std::nullopt, std::string("pending")});
    ASSERT_TRUE(rows);
    pending = static_cast<int>(rows->size());
  }
  EXPECT_EQ(whole->conflicts, pending) << "RunStats::conflicts counts what is left pending";

  const auto same = [&](auto& w, const std::string& what) {
    const std::vector<std::string> got = snapshot(w);
    EXPECT_TRUE(got == want) << what << ": " << rows_difference(got, want);
    check(w, what);
  };
  const auto replayed = [&](auto& w, const std::string& what, int batch_commits) {
    const auto seq = *w.store->latest_change_seq();
    auto again = import(w, batch_commits, std::optional<int>{}, true);
    ASSERT_TRUE(again) << what << ": " << to_string(again.error());
    EXPECT_EQ(*w.store->latest_change_seq(), seq) << what << ": the replay wrote something";
    EXPECT_EQ(again->conflicts, pending) << what << ", replayed in " << batch_commits;
    same(w, what + ", replayed in " + std::to_string(batch_commits));
  };
  replayed(*reference, "largest batches", 500);
  replayed(*reference, "largest batches", 1);
  for (const int batch_commits : {1, 2, 3}) {
    const std::string what = "batches of " + std::to_string(batch_commits);
    auto cut = fresh();
    auto stats = import(*cut, batch_commits, std::optional<int>{}, false);
    ASSERT_TRUE(stats) << what << ": " << to_string(stats.error());
    EXPECT_EQ(stats->conflicts, pending) << what;
    same(*cut, what);
    replayed(*cut, what, batch_commits == 1 ? 500 : 1);

    // Stopped after every batch; each run is a new adapter resuming from the stored token.
    auto resumed = fresh();
    bool finished = false;
    for (int runs = 0; !finished && runs < 300; ++runs) {
      auto one = import(*resumed, batch_commits, std::optional<int>{1}, false);
      ASSERT_TRUE(one) << what << ", run " << runs << ": " << to_string(one.error());
      EXPECT_GE(one->conflicts, 0) << what << ", run " << runs;
      finished = one->finished;
    }
    EXPECT_TRUE(finished) << what;
    same(*resumed, what + ", resumed after every batch");
    for (const int replay_commits : {1, 2, 500})
      replayed(*resumed, what + ", resumed after every batch", replay_commits);
  }
}

}  // namespace pychron::dvc::testing
