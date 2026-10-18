// Table holds, store version and change event of the Spectrometer facade
// (field table editor spec, section 5).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "pychron/systems/spectrometer/field_table_store.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "spectrometer_fakes.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const fs::path kIntegrated = fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "spectrometer.sim-integrated.toml";

// 2026-09-30T12:00:00Z
FieldTableStore::WallTime t0() { return FieldTableStore::WallTime{} + 1790769600s; }

// sim-integrated config and tables, every role a fake; data_root is a fresh
// directory per test unless `with_data_root` is false.
struct TableRig {
  CallLog log;
  ManualClock clock{TimePoint{} + 100s};
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
  FakePositioner positioner{log};
  FakeAcquirer acquirer{{"H2", "H1", "AX", "L1", "L2", "CDD"}};
  fs::path root = unique_root();
  std::unique_ptr<Spectrometer> spec;

  static fs::path unique_root() {
    std::random_device rd;
    return fs::temp_directory_path() / ("pychron_spectables_" + std::to_string(rd()) + std::to_string(rd()));
  }

  TableRig() { fs::create_directories(root); }
  ~TableRig() {
    spec.reset();
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  TableRig(const TableRig&) = delete;
  TableRig& operator=(const TableRig&) = delete;
  TableRig(TableRig&&) = delete;
  TableRig& operator=(TableRig&&) = delete;

  void build(bool with_data_root = true) {
    auto d = cfg::load_spectrometer(kIntegrated);
    ASSERT_TRUE(d.has_value()) << d.error().what;
    std::map<std::string, FieldTable> tables;
    for (const auto& [name, tf] : d->tables) tables.emplace(name, to_field_table(tf));
    SpectrometerRoles roles;
    roles.positioner = &positioner;
    roles.acquirers = {{"sim", &acquirer}};
    Spectrometer::Options options;
    options.sleep = [this](Duration dt) { clock.advance(dt); };
    if (with_data_root) options.data_root = root;
    auto made = Spectrometer::create(d->config, MolecularWeights(d->weights), std::move(tables), std::move(roles),
                                     SpectrometerContext{clock, scheduler, bus}, options);
    ASSERT_TRUE(made.has_value()) << made.error().what;
    spec = std::move(*made);
  }
};

}  // namespace

TEST(SpectrometerTables, HoldIsCountedAndNamed) {
  TableRig r;
  r.build();
  EXPECT_EQ(r.spec->table_held(), std::nullopt);
  auto queue = r.spec->hold_table("queue");
  auto peak = r.spec->hold_table("peak_center");
  EXPECT_EQ(r.spec->table_held(), std::optional<std::string>("queue"));
  queue.release();
  EXPECT_EQ(r.spec->table_held(), std::optional<std::string>("peak_center"));
  queue.release();  // a second release does nothing
  EXPECT_EQ(r.spec->table_held(), std::optional<std::string>("peak_center"));
  peak.release();
  EXPECT_EQ(r.spec->table_held(), std::nullopt);
}

TEST(SpectrometerTables, MovedHoldReleasesOnce) {
  TableRig r;
  r.build();
  auto other = r.spec->hold_table("other");
  {
    Spectrometer::TableHold moved;
    {
      auto first = r.spec->hold_table("queue");
      moved = std::move(first);
    }  // moved-from `first` releases nothing
    EXPECT_EQ(r.spec->table_held(), std::optional<std::string>("other"));
    other.release();
    EXPECT_EQ(r.spec->table_held(), std::optional<std::string>("queue"));
    Spectrometer::TableHold again(std::move(moved));
    EXPECT_EQ(r.spec->table_held(), std::optional<std::string>("queue"));
  }
  EXPECT_EQ(r.spec->table_held(), std::nullopt);
}

TEST(SpectrometerTables, TableVersionReadsStoreCurrent) {
  TableRig r;
  r.build();
  EXPECT_EQ(r.spec->data_root(), r.root);
  const std::string name = r.spec->active_table();
  FieldTableStore store(r.root / "tables", name);
  auto first = store.save(r.spec->field_table(), t0());
  ASSERT_TRUE(first.has_value()) << first.error().what;
  auto second = store.save(r.spec->field_table(), t0() + 60s);
  ASSERT_TRUE(second.has_value()) << second.error().what;
  auto version = r.spec->table_version(name);
  ASSERT_TRUE(version.has_value()) << version.error().what;
  EXPECT_EQ(*version, *second);
  ASSERT_TRUE(store.restore(*first).has_value());
  version = r.spec->table_version(name);
  ASSERT_TRUE(version.has_value()) << version.error().what;
  EXPECT_EQ(*version, *first);
}

TEST(SpectrometerTables, TableVersionWithoutDataRootIsConfigError) {
  TableRig r;
  r.build(false);
  EXPECT_TRUE(r.spec->data_root().empty());
  auto version = r.spec->table_version(r.spec->active_table());
  ASSERT_FALSE(version.has_value());
  EXPECT_EQ(version.error().kind, ErrorKind::Config);
}

TEST(SpectrometerTables, SaveTablePublishesFieldTableChanged) {
  TableRig r;
  r.build();
  std::vector<FieldTableChanged> seen;
  auto sub = r.bus.subscribe<FieldTableChanged>([&seen](const FieldTableChanged& e) { seen.push_back(e); });
  auto saved = r.spec->save_table(t0());
  ASSERT_TRUE(saved.has_value()) << saved.error().what;
  ASSERT_EQ(seen.size(), 1U);
  EXPECT_EQ(seen[0].name, r.spec->active_table());
  EXPECT_EQ(seen[0].version, *saved);
}

TEST(SpectrometerTables, FailedSavePublishesNothing) {
  TableRig r;
  r.build(false);
  std::vector<FieldTableChanged> seen;
  auto sub = r.bus.subscribe<FieldTableChanged>([&seen](const FieldTableChanged& e) { seen.push_back(e); });
  EXPECT_FALSE(r.spec->save_table(t0()).has_value());
  EXPECT_TRUE(seen.empty());
}

// ---- replace_table and restore_table_version --------------------------------

namespace {

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_text(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << text;
}

// The active table with its first value of its first point moved by `delta`.
FieldTable nudged(const FieldTable& table, double delta) {
  FieldTable out = table;
  auto& values = out.mutable_points().front().values;
  values.begin()->second += delta;
  return out;
}

}  // namespace

TEST(SpectrometerTables, ReplaceWritesVersionAndSwapsInMemory) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable next = nudged(r.spec->field_table(), 0.001);
  ASSERT_NE(next, r.spec->field_table());
  auto version = r.spec->replace_table(name, next, "", t0());
  ASSERT_TRUE(version.has_value()) << version.error().what;
  EXPECT_EQ(r.spec->field_table(), next);
  auto current = r.spec->table_version(name);
  ASSERT_TRUE(current.has_value()) << current.error().what;
  EXPECT_EQ(*current, *version);
  auto stored = FieldTableStore(r.root / "tables", name).load(*version);
  ASSERT_TRUE(stored.has_value()) << stored.error().what;
  EXPECT_EQ(*stored, next);
}

TEST(SpectrometerTables, ReplaceRefusesConflict) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto base = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(base.has_value()) << base.error().what;
  FieldTableStore store(r.root / "tables", name);
  auto elsewhere = store.save(nudged(original, 0.002), t0() + 60s);
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().what;

  auto refused = r.spec->replace_table(name, nudged(original, 0.003), *base, t0() + 120s);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_conflict");
  EXPECT_NE(refused.error().what.find(*elsewhere), std::string::npos) << refused.error().what;
  EXPECT_EQ(*r.spec->table_version(name), *elsewhere);
  EXPECT_EQ(r.spec->field_table(), original);

  auto anyway = r.spec->replace_table(name, nudged(original, 0.003), "", t0() + 120s);
  ASSERT_TRUE(anyway.has_value()) << anyway.error().what;
  EXPECT_EQ(*r.spec->table_version(name), *anyway);
  EXPECT_EQ(r.spec->field_table(), nudged(original, 0.003));
}

TEST(SpectrometerTables, ReplaceRefusesAxisMismatch) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto base = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(base.has_value()) << base.error().what;
  ASSERT_EQ(original.axis(), TableAxis::Dac);
  FieldTable other(original.default_fit(), TableAxis::Field, original.points());
  auto refused = r.spec->replace_table(name, other, *base, t0() + 60s);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_axis");
  EXPECT_EQ(*r.spec->table_version(name), *base);
  EXPECT_EQ(r.spec->field_table(), original);
}

TEST(SpectrometerTables, ReplaceRefusesDiagnosticError) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto base = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(base.has_value()) << base.error().what;
  FieldTable dup = original;
  dup.mutable_points().push_back(dup.points().front());
  auto refused = r.spec->replace_table(name, dup, *base, t0() + 60s);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_invalid");
  EXPECT_NE(refused.error().what.find("error: "), std::string::npos) << refused.error().what;
  EXPECT_EQ(*r.spec->table_version(name), *base);
  EXPECT_EQ(r.spec->field_table(), original);
}

TEST(SpectrometerTables, ReplaceRefusedWhileHeldAcceptedAfter) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto base = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(base.has_value()) << base.error().what;
  auto hold = r.spec->hold_table("queue");
  auto refused = r.spec->replace_table(name, nudged(original, 0.001), *base, t0() + 60s);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_held");
  EXPECT_EQ(refused.error().what, "table is held by queue");
  EXPECT_EQ(*r.spec->table_version(name), *base);
  EXPECT_EQ(r.spec->field_table(), original);
  hold.release();
  auto accepted = r.spec->replace_table(name, nudged(original, 0.001), *base, t0() + 60s);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().what;
  EXPECT_EQ(r.spec->field_table(), nudged(original, 0.001));
}

TEST(SpectrometerTables, RestoreRefusedWhileHeldAcceptedAfter) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto first = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(first.has_value()) << first.error().what;
  auto second = r.spec->replace_table(name, nudged(original, 0.001), "", t0() + 60s);
  ASSERT_TRUE(second.has_value()) << second.error().what;
  auto hold = r.spec->hold_table("peak_center");
  auto refused = r.spec->restore_table_version(name, *first);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_held");
  EXPECT_EQ(refused.error().what, "table is held by peak_center");
  EXPECT_EQ(*r.spec->table_version(name), *second);
  EXPECT_EQ(r.spec->field_table(), nudged(original, 0.001));
  hold.release();
  auto accepted = r.spec->restore_table_version(name, *first);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().what;
  EXPECT_EQ(*r.spec->table_version(name), *first);
  EXPECT_EQ(r.spec->field_table(), original);
}

TEST(SpectrometerTables, SaveTableNotRefusedUnderHold) {
  TableRig r;
  r.build();
  auto hold = r.spec->hold_table("queue");
  auto saved = r.spec->save_table(t0());
  ASSERT_TRUE(saved.has_value()) << saved.error().what;
  EXPECT_EQ(*r.spec->table_version(r.spec->active_table()), *saved);
}

TEST(SpectrometerTables, UpdateTableNotRefusedUnderHold) {
  TableRig r;
  r.build();
  const FieldTable original = r.spec->field_table();
  const auto& point = original.points().front();
  const auto& [det, value] = *point.values.begin();
  auto hold = r.spec->hold_table("queue");
  auto updated = r.spec->update_table(det, point.isotope, value + 0.001, false);
  ASSERT_TRUE(updated.has_value()) << updated.error().what;
  EXPECT_EQ(r.spec->field_table().points().front().values.at(det), value + 0.001);
}

TEST(SpectrometerTables, RestoreReloadsInMemory) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto first = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(first.has_value()) << first.error().what;
  auto second = r.spec->replace_table(name, nudged(original, 0.004), "", t0() + 60s);
  ASSERT_TRUE(second.has_value()) << second.error().what;
  ASSERT_EQ(r.spec->field_table(), nudged(original, 0.004));
  auto restored = r.spec->restore_table_version(name, *first);
  ASSERT_TRUE(restored.has_value()) << restored.error().what;
  EXPECT_EQ(r.spec->field_table(), original);
  EXPECT_EQ(*r.spec->table_version(name), *first);
}

TEST(SpectrometerTables, RestoreRefusesVersionWithDiagnosticError) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto base = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(base.has_value()) << base.error().what;
  FieldTable dup = original;
  dup.mutable_points().push_back(dup.points().front());
  const fs::path dir = r.root / "tables" / name;
  const std::string bad = "20260101T000000Z";
  write_text(dir / (bad + ".toml"), to_toml(dup));
  const std::string pointer = read_text(dir / "current");

  auto refused = r.spec->restore_table_version(name, bad);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_invalid");
  EXPECT_NE(refused.error().what.find("error: "), std::string::npos) << refused.error().what;
  EXPECT_EQ(read_text(dir / "current"), pointer);
  EXPECT_EQ(r.spec->field_table(), original);
}

TEST(SpectrometerTables, RestoreRefusesUnparseableVersion) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  auto base = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(base.has_value()) << base.error().what;
  const fs::path dir = r.root / "tables" / name;
  const std::string bad = "20260101T000000Z";
  write_text(dir / (bad + ".toml"), "this is = = not toml [[\n");
  const std::string pointer = read_text(dir / "current");

  auto refused = r.spec->restore_table_version(name, bad);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, "table_invalid");
  EXPECT_EQ(read_text(dir / "current"), pointer);
  EXPECT_EQ(r.spec->field_table(), original);
}

TEST(SpectrometerTables, BothPublishFieldTableChanged) {
  TableRig r;
  r.build();
  const std::string name = r.spec->active_table();
  const FieldTable original = r.spec->field_table();
  std::vector<FieldTableChanged> seen;
  auto sub = r.bus.subscribe<FieldTableChanged>([&seen](const FieldTableChanged& e) { seen.push_back(e); });
  auto first = r.spec->replace_table(name, original, "", t0());
  ASSERT_TRUE(first.has_value()) << first.error().what;
  auto second = r.spec->replace_table(name, nudged(original, 0.001), *first, t0() + 60s);
  ASSERT_TRUE(second.has_value()) << second.error().what;
  // A refused change publishes nothing.
  EXPECT_FALSE(r.spec->replace_table(name, original, *first, t0() + 120s).has_value());
  ASSERT_TRUE(r.spec->restore_table_version(name, *first).has_value());
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0].name, name);
  EXPECT_EQ(seen[0].version, *first);
  EXPECT_EQ(seen[1].version, *second);
  EXPECT_EQ(seen[2].name, name);
  EXPECT_EQ(seen[2].version, *first);
}
