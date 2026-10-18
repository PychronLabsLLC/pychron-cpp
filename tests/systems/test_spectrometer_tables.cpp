// Table holds, store version and change event of the Spectrometer facade
// (field table editor spec, section 5).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
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
