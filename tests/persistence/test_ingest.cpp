// Idempotent ingest (DVC schema spec, sections 5.3, 7.3, 8.3; invariants I3,
// I7, I13).

#include <gtest/gtest.h>

#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

class IngestTest : public StoreTest {};

}  // namespace

TEST_P(IngestTest, CreatesOneCollectionChangesetWithRootHeadsForEveryKind) {
  const Bytes signal = series(1), baseline = series(0);
  const auto item = analysis_item(lab_, 7, signal, baseline);
  const auto& a = std::get<AnalysisIngest>(item.body);
  auto ack = store_->ingest(item);
  ASSERT_TRUE(ack) << to_string(ack.error());
  EXPECT_FALSE(ack->duplicate);
  ASSERT_TRUE(ack->change_seq);

  auto heads = store_->heads(a.analysis);
  ASSERT_TRUE(heads);
  ASSERT_EQ(heads->size(), std::size(kCollectionKinds));
  for (Kind kind : kCollectionKinds) {
    auto history = store_->history(a.analysis, kind);
    ASSERT_TRUE(history) << to_string(history.error());
    ASSERT_EQ(history->size(), 1u) << to_string(kind);
    const auto& root = history->front();
    EXPECT_FALSE(root.parent) << "root revisions have no parent (I3)";
    EXPECT_EQ(root.changeset.uuid, a.changeset);
    EXPECT_EQ(root.changeset.kind, ChangesetKind::Collection);
    EXPECT_EQ(root.changeset.author_user, lab_.analyst);
    EXPECT_EQ(root.changeset.client, lab_.acquisition_client);
    EXPECT_EQ(root.changeset.message, "<COLLECTION> 66573-07");
    EXPECT_EQ(root.change_seq, *ack->change_seq);
  }

  auto view = store_->load_analysis(a.analysis);
  ASSERT_TRUE(view);
  ASSERT_TRUE(*view);
  EXPECT_EQ((*view)->summary.runid, "66573-07");
  EXPECT_EQ((*view)->summary.timestamp, a.timestamp);
  EXPECT_EQ((*view)->summary.mass_spectrometer, "jan");
  EXPECT_EQ((*view)->summary.signals_state, "pending");
  EXPECT_EQ(std::get<Intercepts>((*view)->payloads.at(Kind::Intercepts)), a.roots.intercepts_rows);
  EXPECT_EQ(std::get<Baselines>((*view)->payloads.at(Kind::Baselines)), a.roots.baselines_rows);
  EXPECT_EQ(std::get<Blanks>((*view)->payloads.at(Kind::Blanks)), a.roots.blanks_rows);
  EXPECT_EQ(std::get<IcFactors>((*view)->payloads.at(Kind::IcFactors)), a.roots.icfactors_rows);
  EXPECT_EQ(std::get<SignalRefs>((*view)->payloads.at(Kind::Signals)), a.roots.signal_refs);
  EXPECT_EQ(std::get<TagValue>((*view)->payloads.at(Kind::Tags)).name, "ok");
}

TEST_P(IngestTest, SecondIngestOfTheSameItemIsAnIdempotentAck) {
  const auto item = analysis_item(lab_, 1, series(1), series(0));
  auto first = store_->ingest(item);
  ASSERT_TRUE(first) << to_string(first.error());
  auto second = store_->ingest(item);
  ASSERT_TRUE(second) << to_string(second.error());
  EXPECT_TRUE(second->duplicate);
  EXPECT_EQ(second->change_seq, first->change_seq);
  auto found = store_->find_analyses(AnalysisQuery{"66573", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  ASSERT_TRUE(found);
  EXPECT_EQ(found->size(), 1u);
}

TEST_P(IngestTest, SameKeyWithADifferentPayloadIsRejected) {
  auto item = analysis_item(lab_, 1, series(1), series(0));
  ASSERT_TRUE(store_->ingest(item));
  item.payload_sha256 = sha256(std::string_view{"something else"});
  auto again = store_->ingest(item);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().kind, ErrorKind::Protocol);
  EXPECT_NE(again.error().what.find("IdempotencyMismatch"), std::string::npos);
}

TEST_P(IngestTest, UnknownIdentifierIsAPermanentErrorAndWritesNothing) {
  auto item = analysis_item(lab_, 1, series(1), series(0));
  std::get<AnalysisIngest>(item.body).identifier = "99999";
  auto before = store_->changes_since(0, 1000);
  auto ack = store_->ingest(item);
  ASSERT_FALSE(ack);
  EXPECT_EQ(ack.error().kind, ErrorKind::Protocol);
  auto after = store_->changes_since(0, 1000);
  EXPECT_EQ(after->entries.size(), before->entries.size());
  auto found = store_->find_analyses(AnalysisQuery{});
  EXPECT_TRUE(found->empty());
}

TEST_P(IngestTest, DuplicateRunIdIsRejected) {
  ASSERT_TRUE(store_->ingest(analysis_item(lab_, 3, series(1), series(0))));
  auto clash = store_->ingest(analysis_item(lab_, 3, series(2), series(0)));
  ASSERT_FALSE(clash);  // UNIQUE (identifier, aliquot, increment), I9
  EXPECT_EQ(clash.error().kind, ErrorKind::Protocol);
}

TEST_P(IngestTest, SignalsCompleteWhenTheLastBlobArrives) {
  const Bytes signal = series(1), baseline = series(0);
  const auto item = analysis_item(lab_, 1, signal, baseline);
  const Uuid analysis = std::get<AnalysisIngest>(item.body).analysis;
  ASSERT_TRUE(store_->ingest(item));

  auto first = store_->ingest(IngestItem{Uuid::v7(), {}, lab_.acquisition_client, BlobIngest{"f32le-tv/1", signal, 4}});
  ASSERT_TRUE(first) << to_string(first.error());
  EXPECT_FALSE(first->change_seq) << "one blob still missing: no completion yet";
  EXPECT_EQ((*store_->load_analysis(analysis))->summary.signals_state, "pending");

  auto second =
      store_->ingest(IngestItem{Uuid::v7(), {}, lab_.acquisition_client, BlobIngest{"f32le-tv/1", baseline, 4}});
  ASSERT_TRUE(second) << to_string(second.error());
  ASSERT_TRUE(second->change_seq);
  EXPECT_EQ((*store_->load_analysis(analysis))->summary.signals_state, "complete");
  auto page = store_->changes_since(*second->change_seq - 1, 10);
  ASSERT_EQ(page->entries.size(), 1u);
  EXPECT_EQ(page->entries[0].kind, "blob_complete");
  ASSERT_EQ(page->entries[0].entities.size(), 1u);
  EXPECT_EQ(page->entries[0].entities[0].entity, analysis);

  auto dup = store_->ingest(IngestItem{Uuid::v7(), {}, lab_.acquisition_client, BlobIngest{"f32le-tv/1", signal, 4}});
  ASSERT_TRUE(dup);
  EXPECT_TRUE(dup->duplicate);
  EXPECT_FALSE(dup->change_seq);
}

TEST_P(IngestTest, AnalysisWhoseBlobsAreAlreadyPresentIsCompleteAtIngest) {
  const Bytes signal = series(1), baseline = series(0);
  for (const Bytes* b : {&signal, &baseline})
    ASSERT_TRUE(store_->ingest(IngestItem{Uuid::v7(), {}, lab_.acquisition_client, BlobIngest{"f32le-tv/1", *b, 4}}));
  const auto item = analysis_item(lab_, 1, signal, baseline);
  ASSERT_TRUE(store_->ingest(item));
  EXPECT_EQ((*store_->load_analysis(std::get<AnalysisIngest>(item.body).analysis))->summary.signals_state, "complete");
}

TEST_P(IngestTest, CreatesMissingUserAndLoadByNaturalKey) {
  auto item = analysis_item(lab_, 1, series(1), series(0));
  std::get<AnalysisIngest>(item.body).analyst = "new-analyst";
  auto ack = store_->ingest(item);
  ASSERT_TRUE(ack) << to_string(ack.error());
  auto page = store_->changes_since(*ack->change_seq - 1, 10);
  ASSERT_EQ(page->entries.size(), 1u);
  std::set<std::string> types;
  for (const auto& e : page->entries[0].entities) types.insert(e.entity_type);
  EXPECT_EQ(types, (std::set<std::string>{"analysis", "app_user", "load"}));
}

TEST_P(IngestTest, SatellitesAreStoredAndTheirBlobsGateCompletion) {
  // Raw rows are checked through a second connection: file-backed database.
  TestDatabase shared(GetParam(), true);
  auto store = open_or_die(shared.url());
  ASSERT_TRUE(store);
  const Lab lab = seed_lab(*store);
  const Bytes signal = series(1), baseline = series(0), peak = series(7, 20);
  auto item = analysis_item(lab, 1, signal, baseline);
  auto& a = std::get<AnalysisIngest>(item.body);
  a.meta = AnalysisMetaRow{};
  a.meta->source_json = R"({"emission": 200})";
  a.meta->intensity_scalar = 1.0;
  PeakCenterRow pc;
  pc.detector = "H1";
  pc.center_dac = 5.432;
  pc.points_blob_sha = blob_sha256(kCodecTv, peak);
  a.peak_centers = {pc};
  MonitorCheckRow mc;
  mc.ordinal = 0;
  mc.name = "AxialCheck";
  mc.tripped = false;
  a.monitor_checks = {mc};
  a.artifacts = {{"run.log", "log", std::nullopt, "https://logs.example/run.log"}};
  a.measured_positions = {{"load-1", 3, 1.5, -2.0, 0.0, false}};
  a.spectrometer_snapshot = SpectrometerSnapshot{"abc123", R"({"magnet": 1})", R"({"H1": 1.0})", "{}", "{}"};
  a.scripts.measurement = "def main():\n    multicollect()\n";
  a.scripts.extraction = "def main():\n    extract()\n";
  a.queue = QueueRow{Uuid::v7(), "queue-1", "jross", std::nullopt, 1};
  ASSERT_TRUE(store->ingest(item));

  // Same scripts and queue in a second analysis are stored once.
  auto item2 = analysis_item(lab, 2, signal, baseline);
  auto& a2 = std::get<AnalysisIngest>(item2.body);
  a2.scripts = a.scripts;
  a2.queue = a.queue;
  a2.spectrometer_snapshot = a.spectrometer_snapshot;
  ASSERT_TRUE(store->ingest(item2));

  auto db = std::move(*pd::Db::open(StoreConfig{shared.url(), false}));
  auto count = [&](const char* sql) { return (*db->select_one(sql))->value("n").toInt(); };
  EXPECT_EQ(count("SELECT count(*) AS n FROM script_text"), 2);
  EXPECT_EQ(count("SELECT count(*) AS n FROM experiment_queue"), 1);
  EXPECT_EQ(count("SELECT count(*) AS n FROM spectrometer_snapshot"), 1);
  EXPECT_EQ(count("SELECT count(*) AS n FROM analysis WHERE measurement_script_sha IS NOT NULL "
                  "AND spectrometer_snapshot_sha IS NOT NULL AND queue_uuid IS NOT NULL"),
            2);
  EXPECT_EQ(count("SELECT count(*) AS n FROM analysis_meta"), 1);
  EXPECT_EQ(count("SELECT count(*) AS n FROM peak_center"), 1);
  EXPECT_EQ(count("SELECT count(*) AS n FROM monitor_check"), 1);
  EXPECT_EQ(count("SELECT count(*) AS n FROM analysis_artifact"), 1);
  EXPECT_EQ(count("SELECT count(*) AS n FROM measured_position WHERE position = 3"), 1);
  auto snap = db->select_one("SELECT sha256 FROM spectrometer_snapshot");
  EXPECT_EQ(pd::to_digest((*snap)->value("sha256")), snapshot_sha256(*a.spectrometer_snapshot));

  // Signal and baseline blobs alone do not complete the first analysis: its
  // peak-center points are referenced too (I13). The second has no peak center.
  for (const Bytes* b : {&signal, &baseline})
    ASSERT_TRUE(store->ingest(IngestItem{Uuid::v7(), {}, lab.acquisition_client, BlobIngest{"f32le-tv/1", *b, 4}}));
  EXPECT_EQ((*store->load_analysis(a.analysis))->summary.signals_state, "pending");
  EXPECT_EQ((*store->load_analysis(a2.analysis))->summary.signals_state, "complete");
  ASSERT_TRUE(store->ingest(IngestItem{Uuid::v7(), {}, lab.acquisition_client, BlobIngest{"f32le-tv/1", peak, 20}}));
  EXPECT_EQ((*store->load_analysis(a.analysis))->summary.signals_state, "complete");
}

INSTANTIATE_TEST_SUITE_P(Engines, IngestTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
