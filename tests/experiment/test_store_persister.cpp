#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <QCoreApplication>
#include <QThread>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include "failing_store.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/store/record_ingest.hpp"
#include "pychron/experiment/store/store_persister.hpp"
#include "store_fixture.hpp"
#include "store_records.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::testing;
namespace ps = pychron::persistence;
namespace pt = pychron::persistence::testing;
namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
  auto dir = fs::temp_directory_path() / ("pychron_persister_" + pt::TestDatabase::unique_tag());
  fs::create_directories(dir);
  return dir;
}

RunSpec run(std::string identifier, std::optional<int> aliquot = std::nullopt, bool skip = false) {
  RunSpec r;
  r.id.identifier = std::move(identifier);
  r.id.aliquot = aliquot;
  r.skip = skip;
  return r;
}

QueueSpec queue(std::string mass_spectrometer, std::string device, std::vector<RunSpec> runs) {
  QueueSpec q;
  q.mass_spectrometer = std::move(mass_spectrometer);
  q.extract_device = std::move(device);
  q.runs = std::move(runs);
  return q;
}

record::AnalysisRecord record_with_device(const std::string& device) {
  auto b = sample_builder();
  b.set_instrument({"jan", device, "lab", "jdoe", "0.1.0", "abc123"});
  auto r = b.finalize();
  EXPECT_TRUE(r.has_value());
  return r.value();
}

// A sample record with its own uuid: two runs of one identifier are two items
// to the store, and one uuid is one item.
record::AnalysisRecord numbered(const std::string& identifier = "66573", int aliquot = 1,
                                const std::string& mass_spectrometer = "jan") {
  auto r = sample_record(identifier, aliquot, mass_spectrometer);
  r.identity.uuid = ps::Uuid::v5(ps::Uuid{}, identifier + "/" + std::to_string(aliquot)).str();
  return r;
}

// A run that measured nothing: no series, so no raw blob to wait for.
record::AnalysisRecord record_without_series() {
  auto r = numbered();
  r.data.series.clear();
  r.results.intercepts.clear();
  r.results.baselines.clear();
  return r;
}

class StorePersisterTest : public pt::StoreTest {
 protected:
  void SetUp() override {
    StoreTest::SetUp();
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) return;
    ASSERT_TRUE(store_->add_extract_device(lab_.acquisition_client, "co2"));
    data_ = scratch_dir();
    failing_ = std::make_unique<FailingStore>(*store_);
    persister_ = std::make_unique<store::StorePersister>(*failing_, lab_.acquisition_client, data_);
  }
  void TearDown() override {
    persister_.reset();
    failing_.reset();
    std::error_code ignored;
    fs::remove_all(data_, ignored);
  }

  std::size_t analyses_of(const std::string& identifier) {
    auto found = store_->find_analyses({.identifier = identifier});
    EXPECT_TRUE(found.has_value());
    return found ? found->size() : 0;
  }
  std::string save_error(const record::AnalysisRecord& r) {
    auto saved = persister_->save_analysis(r);
    EXPECT_FALSE(saved.has_value());
    return saved ? "" : saved.error().what;
  }

  fs::path data_;
  std::unique_ptr<FailingStore> failing_;
  std::unique_ptr<store::StorePersister> persister_;
};

}  // namespace

TEST_P(StorePersisterTest, ASavedRecordIsAnAnalysisWithItsSignals) {
  ASSERT_TRUE(persister_->save_analysis(numbered()));
  auto id = store_->find_analysis("66573", 1, -1);
  ASSERT_TRUE(id.has_value());
  ASSERT_TRUE(id->has_value());
  auto view = store_->load_analysis(**id);
  ASSERT_TRUE(view.has_value());
  ASSERT_TRUE(view->has_value());
  std::set<ps::Kind> kinds;
  for (const auto& h : (*view)->heads) kinds.insert(h.kind);
  for (auto k : {ps::Kind::Signals, ps::Kind::Intercepts, ps::Kind::Baselines, ps::Kind::Tags})
    EXPECT_TRUE(kinds.contains(k)) << static_cast<int>(k);
  auto found = store_->find_analyses({.identifier = "66573"});
  ASSERT_TRUE(found.has_value());
  ASSERT_EQ(found->size(), 1U);
  EXPECT_EQ((*found)[0].signals_state, "complete");
}

TEST_P(StorePersisterTest, SavingTwiceIsOneAnalysis) {
  ASSERT_TRUE(persister_->save_analysis(numbered()));
  ASSERT_TRUE(persister_->save_analysis(numbered()));
  EXPECT_EQ(analyses_of("66573"), 1U);
}

TEST_P(StorePersisterTest, ARecordWithNoSeriesIsComplete) {
  ASSERT_TRUE(persister_->save_analysis(record_without_series()));
  auto found = store_->find_analyses({.identifier = "66573"});
  ASSERT_TRUE(found.has_value());
  ASSERT_EQ(found->size(), 1U);
  EXPECT_EQ((*found)[0].signals_state, "complete");
}

TEST_P(StorePersisterTest, AnUnknownIdentifierIsRefusedAndSavesOnceItExists) {
  const auto before = store_->latest_change_seq();
  ASSERT_TRUE(before.has_value());
  auto refused = persister_->save_analysis(numbered("77000"));
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().what.find("not in the database"), std::string::npos) << refused.error().what;
  EXPECT_NE(refused.error().what.find("77000"), std::string::npos) << refused.error().what;
  // The prefix does not hide what the error is from a caller that tests for it.
  EXPECT_TRUE(ps::is_unknown_catalog_reference(refused.error())) << refused.error().what;
  EXPECT_EQ(analyses_of("77000"), 0U);
  EXPECT_EQ(*store_->latest_change_seq(), *before);

  ASSERT_TRUE(store_->add_identifier(lab_.acquisition_client,
                                     {"77000", "unknown", std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                      std::nullopt}));
  ASSERT_TRUE(persister_->save_analysis(numbered("77000")));
  EXPECT_EQ(analyses_of("77000"), 1U);
}

TEST_P(StorePersisterTest, AnUnknownMassSpectrometerIsRefusedAndSavesOnceItExists) {
  auto refused = persister_->save_analysis(numbered("66573", 1, "nope"));
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().what.find("not in the database"), std::string::npos) << refused.error().what;
  EXPECT_NE(refused.error().what.find("nope"), std::string::npos) << refused.error().what;
  EXPECT_TRUE(ps::is_unknown_catalog_reference(refused.error()));
  EXPECT_EQ(analyses_of("66573"), 0U);

  ASSERT_TRUE(store_->add_mass_spectrometer(lab_.acquisition_client, {"nope", "argus", "n", std::nullopt}));
  ASSERT_TRUE(persister_->save_analysis(numbered("66573", 1, "nope")));
  EXPECT_EQ(analyses_of("66573"), 1U);
}

TEST_P(StorePersisterTest, AnUnknownExtractDeviceIsRefusedAndSavesOnceItExists) {
  const std::string what = save_error(record_with_device("laser9"));
  EXPECT_NE(what.find("not in the database"), std::string::npos) << what;
  EXPECT_NE(what.find("laser9"), std::string::npos) << what;
  EXPECT_EQ(analyses_of("66573"), 0U);

  ASSERT_TRUE(store_->add_extract_device(lab_.acquisition_client, "laser9"));
  ASSERT_TRUE(persister_->save_analysis(record_with_device("laser9")));
  EXPECT_EQ(analyses_of("66573"), 1U);
}

TEST_P(StorePersisterTest, AliquotsFollowTheStore) {
  ASSERT_TRUE(persister_->save_analysis(numbered("66573", 1)));
  ASSERT_TRUE(persister_->save_analysis(numbered("66573", 2)));
  const fs::path other = scratch_dir();
  store::StorePersister fresh(*failing_, lab_.acquisition_client, other);
  EXPECT_EQ(fresh.next_aliquot("66573").value(), 3);
  EXPECT_EQ(fresh.next_aliquot("66573").value(), 4);
  EXPECT_EQ(fresh.next_aliquot("66574").value(), 1);
  std::error_code ignored;
  fs::remove_all(other, ignored);
}

TEST_P(StorePersisterTest, AliquotsCountARecordWaitingInTheSpool) {
  persist::Spool spool(data_ / "spool");
  ASSERT_TRUE(spool.put(numbered("66573", 5)));
  EXPECT_EQ(persister_->next_aliquot("66573").value(), 6);
  EXPECT_EQ(persister_->next_aliquot("66574").value(), 1);  // another identifier's record is not counted
}

TEST_P(StorePersisterTest, AliquotsSurviveAnUnreadableStore) {
  EXPECT_EQ(persister_->next_aliquot("66573").value(), 1);
  failing_->fail_reads(true);
  EXPECT_EQ(persister_->next_aliquot("66573").value(), 2);

  const fs::path other = scratch_dir();
  store::StorePersister fresh(*failing_, lab_.acquisition_client, other);
  EXPECT_FALSE(fresh.next_aliquot("66573").has_value());
  std::error_code ignored;
  fs::remove_all(other, ignored);
}

TEST_P(StorePersisterTest, CheckQueueNamesExactlyWhatIsMissing) {
  auto lines = persister_->check_queue(queue("nope", "laser9", {run("66573"), run("BU"), run("BU"), run("66574")}));
  ASSERT_TRUE(lines.has_value());
  ASSERT_EQ(lines->size(), 3U) << ::testing::PrintToString(*lines);
  EXPECT_EQ((*lines)[0], "mass spectrometer 'nope' is not in the database");
  EXPECT_EQ((*lines)[1], "extract device 'laser9' is not in the database");
  EXPECT_EQ((*lines)[2], "identifier 'BU' is not in the database");

  auto none = persister_->check_queue(queue("jan", "co2", {run("66573")}));
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->empty()) << ::testing::PrintToString(*none);
}

TEST_P(StorePersisterTest, CheckQueueSaysWhatAnEmptyQueueNamesNothing) {
  auto lines = persister_->check_queue(queue("", "", {run("66573")}));
  ASSERT_TRUE(lines.has_value());
  ASSERT_EQ(lines->size(), 1U) << ::testing::PrintToString(*lines);
  EXPECT_EQ((*lines)[0], "the queue names no mass spectrometer");
}

TEST_P(StorePersisterTest, CheckQueueLeavesSkippedRunsAlone) {
  auto lines = persister_->check_queue(queue("jan", "", {run("BU", std::nullopt, true), run("66573")}));
  ASSERT_TRUE(lines.has_value());
  EXPECT_TRUE(lines->empty()) << ::testing::PrintToString(*lines);
}

TEST_P(StorePersisterTest, CheckQueueNamesATakenRunId) {
  ASSERT_TRUE(persister_->save_analysis(numbered("66573", 1)));
  auto lines = persister_->check_queue(queue("jan", "co2", {run("66573", 1), run("66573", 2)}));
  ASSERT_TRUE(lines.has_value());
  ASSERT_EQ(lines->size(), 1U) << ::testing::PrintToString(*lines);
  EXPECT_EQ((*lines)[0], "66573-01 is already in the database");
}

// A queue is checked as it will run: rows before the one it starts at are
// not run again, so what the database already holds of them is no objection.
TEST_P(StorePersisterTest, AQueueStartedPastATakenRunIdIsAccepted) {
  ASSERT_TRUE(persister_->save_analysis(numbered("66573", 1)));
  const auto q = queue("jan", "co2", {run("66573", 1), run("66573", 2)});
  auto from_the_top = persister_->check_queue(persist::rows_from(q, 0));
  ASSERT_TRUE(from_the_top.has_value());
  EXPECT_EQ(*from_the_top, std::vector<std::string>{"66573-01 is already in the database"});
  auto from_the_second = persister_->check_queue(persist::rows_from(q, 1));
  ASSERT_TRUE(from_the_second.has_value());
  EXPECT_TRUE(from_the_second->empty()) << ::testing::PrintToString(*from_the_second);
  // Past the end: nothing will run.
  auto past = persister_->check_queue(persist::rows_from(q, 9));
  ASSERT_TRUE(past.has_value());
  EXPECT_TRUE(past->empty());
}

// A step the store cannot hold is known before the run: it is not measured
// and then kept in the spool for good.
TEST_P(StorePersisterTest, CheckQueueRefusesAStepThatIsNotLetters) {
  auto bad = run("66573");
  bad.id.step = "A1";
  auto skipped = run("66573", std::nullopt, true);
  skipped.id.step = "7";
  auto fine = run("66573");
  fine.id.step = "b";
  auto lines = persister_->check_queue(queue("jan", "co2", {bad, bad, skipped, fine}));
  ASSERT_TRUE(lines.has_value());
  EXPECT_EQ(*lines, std::vector<std::string>{"66573: step 'A1' is not letters: the database cannot hold it"});
}

TEST_P(StorePersisterTest, CheckQueueNamesATakenRunIdOnce) {
  ASSERT_TRUE(persister_->save_analysis(numbered("66573", 1)));
  const auto before = failing_->reads();
  auto lines = persister_->check_queue(queue("jan", "co2", {run("66573", 1), run("66573", 1)}));
  ASSERT_TRUE(lines.has_value());
  ASSERT_EQ(lines->size(), 1U) << ::testing::PrintToString(*lines);
  EXPECT_EQ((*lines)[0], "66573-01 is already in the database");
  // mass spectrometer, extract device, identifier (once), run id (once)
  EXPECT_EQ(failing_->reads() - before, 4);
}

TEST_P(StorePersisterTest, CheckQueueSaysOnlyThatTheDatabaseIsGoneWhenALaterReadFails) {
  // Reads: jan (found), BU (missing: a line is collected), 66573 (fails).
  failing_->fail_reads_after(2);
  auto lines = persister_->check_queue(queue("jan", "", {run("BU"), run("66573")}));
  ASSERT_TRUE(lines.has_value());
  ASSERT_EQ(lines->size(), 1U) << ::testing::PrintToString(*lines);
  EXPECT_EQ((*lines)[0].rfind("the database cannot be reached:", 0), 0U) << (*lines)[0];
}

TEST_P(StorePersisterTest, AliquotsRefuseASpoolThatCannotBeListed) {
#ifdef _WIN32
  GTEST_SKIP() << "directory permissions are not portable to Windows";
#else
  const fs::path spool_dir = data_ / "spool";
  fs::create_directories(spool_dir);
  fs::permissions(spool_dir, fs::perms::none);
  const bool listed = persist::Spool(spool_dir).pending().has_value();
  if (listed) {
    fs::permissions(spool_dir, fs::perms::owner_all);
    GTEST_SKIP() << "this user can list an unreadable directory (root?)";
  }
  auto aliquot = persister_->next_aliquot("66573");
  fs::permissions(spool_dir, fs::perms::owner_all);
  EXPECT_FALSE(aliquot.has_value());
#endif
}

TEST_P(StorePersisterTest, AliquotsSkipASpooledRecordThatCannotBeRead) {
  fs::create_directories(data_ / "spool");
  std::ofstream(data_ / "spool" / "junk.json") << "{ not a record";
  EXPECT_EQ(persister_->next_aliquot("66573").value(), 1);
}

TEST_P(StorePersisterTest, CheckQueueSaysWhenTheDatabaseCannotBeReached) {
  failing_->fail_reads(true);
  auto lines = persister_->check_queue(queue("nope", "laser9", {run("BU")}));
  ASSERT_TRUE(lines.has_value());
  ASSERT_EQ(lines->size(), 1U) << ::testing::PrintToString(*lines);
  EXPECT_EQ((*lines)[0].rfind("the database cannot be reached:", 0), 0U) << (*lines)[0];
}

TEST_P(StorePersisterTest, AFailedSaveStaysInTheSpoolAndIsSentLater) {
  persist::Spool spool(data_ / "spool");
  persist::SavePipeline pipeline(spool, *persister_);
  failing_->fail_ingest(10);
  ASSERT_TRUE(pipeline.save(numbered()));
  EXPECT_EQ(pipeline.pending(), 1U);
  EXPECT_TRUE(pipeline.last_error().has_value());
  EXPECT_EQ(analyses_of("66573"), 0U);

  failing_->fail_ingest(0);
  auto sent = pipeline.recover();
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(*sent, 1U);
  EXPECT_EQ(pipeline.pending(), 0U);
  EXPECT_EQ(analyses_of("66573"), 1U);
}

TEST_P(StorePersisterTest, ACrashAfterTheStoresCommitLeavesOneAnalysis) {
  const auto items = store::to_ingest(numbered(), lab_.acquisition_client);
  ASSERT_TRUE(items.has_value());
  const int n = static_cast<int>(items->blobs.size()) + 1;
  persist::Spool spool(data_ / "spool");
  persist::SavePipeline pipeline(spool, *persister_);
  failing_->fail_after_commit(n);
  ASSERT_TRUE(pipeline.save(numbered()));
  EXPECT_EQ(pipeline.pending(), 1U);
  for (int pass = 0; pass <= n && pipeline.pending() > 0; ++pass) (void)pipeline.recover();
  EXPECT_EQ(pipeline.pending(), 0U);
  EXPECT_EQ(analyses_of("66573"), 1U);
}

TEST_P(StorePersisterTest, ADraftRecordStaysInTheSpool) {
  persist::Spool spool(data_ / "spool");
  persist::SavePipeline pipeline(spool, *persister_);
  ASSERT_TRUE(pipeline.save(sample_builder().draft()));
  EXPECT_EQ(pipeline.pending(), 1U);
  (void)pipeline.recover();
  EXPECT_EQ(pipeline.pending(), 1U);
  EXPECT_EQ(analyses_of("66573"), 0U);
}

INSTANTIATE_TEST_SUITE_P(Engines, StorePersisterTest, ::testing::ValuesIn(pt::engines()));

// ---------------------------------------------------------------- Acquisition

namespace {

// Joins on every path out of a test, so a failed assertion cannot leave a
// running thread behind.
struct Joined {
  std::thread thread;
  Joined() = default;
  explicit Joined(std::thread t) : thread(std::move(t)) {}
  Joined(const Joined&) = delete;
  Joined& operator=(const Joined&) = delete;
  ~Joined() {
    if (thread.joinable()) thread.join();
  }
};

// A database with the lab seeded, on the engine under test, file backed so
// that several handles see it.
class AcquisitionTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    tdb_ = std::make_unique<pt::TestDatabase>(GetParam(), true);
    auto seed = pt::open_or_die(tdb_->url());
    ASSERT_TRUE(seed);
    const auto lab = pt::seed_lab(*seed);
    ASSERT_TRUE(seed->add_extract_device(lab.acquisition_client, "co2"));
    data_ = scratch_dir();
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(data_, ignored);
  }

  std::unique_ptr<store::Acquisition> open(const char* sub = "a") {
    auto a = store::open_acquisition(tdb_->url(), data_ / sub, "test");
    EXPECT_TRUE(a.has_value()) << (a ? "" : a.error().what);
    return a ? std::move(*a) : nullptr;
  }
  std::size_t analyses_of(const std::string& identifier) {
    auto reader = pt::open_or_die(tdb_->url());
    if (!reader) return 0;
    auto found = reader->find_analyses({.identifier = identifier});
    EXPECT_TRUE(found.has_value());
    return found ? found->size() : 0;
  }

  std::unique_ptr<pt::TestDatabase> tdb_;
  fs::path data_;
};

}  // namespace

TEST_P(AcquisitionTest, ServesCallsFromOtherThreads) {
  auto acquisition = open();
  ASSERT_TRUE(acquisition);
  auto& persister = acquisition->persister();
  std::string error;
  int first = 0;
  std::vector<std::string> lines{"unset"};
  {
    Joined caller(std::thread([&] {
      auto checked = persister.check_queue(queue("jan", "co2", {run("66573")}));
      if (!checked) return void(error = checked.error().what);
      lines = *checked;
      auto n = persister.next_aliquot("66573");
      if (!n) return void(error = n.error().what);
      first = *n;
      if (auto saved = persister.save_analysis(numbered("66573", first)); !saved) error = saved.error().what;
    }));
  }
  EXPECT_EQ(error, "");
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(first, 1);
  EXPECT_EQ(persister.next_aliquot("66573").value(), 2);
  EXPECT_EQ(analyses_of("66573"), 1U);
}

namespace {
using Clock = std::chrono::steady_clock;
// A real-time upper bound: it only tells "done" from "stuck".
constexpr auto kBound = std::chrono::seconds(30);
}  // namespace

TEST_P(AcquisitionTest, ServesTwoCallersAtOnce) {
  const auto begin = Clock::now();
  auto acquisition = open();
  ASSERT_TRUE(acquisition);
  auto& persister = acquisition->persister();
  std::atomic<int> failures{0};
  const auto writer = [&](int from) {
    for (int i = from; i < from + 20; ++i)
      if (!persister.save_analysis(numbered("66573", i))) ++failures;
  };
  {
    Joined a(std::thread(writer, 1));
    Joined b(std::thread(writer, 21));
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(analyses_of("66573"), 40U);
  EXPECT_LT(Clock::now() - begin, kBound);
}

// A writer saves through the acquisition while another handle, on this
// thread, reads: the reader does not hold the writer up (spec 8.5).
TEST_P(AcquisitionTest, AReaderDoesNotLockOutTheWriter) {
  auto acquisition = open();
  ASSERT_TRUE(acquisition);
  auto reader = pt::open_or_die(tdb_->url());
  ASSERT_TRUE(reader);
  constexpr int kRuns = 50;
  std::atomic<bool> done{false};
  std::string write_error, read_error;
  int reads = 0;
  const auto deadline = Clock::now() + kBound;
  {
    Joined writer(std::thread([&] {
      for (int i = 1; i <= kRuns; ++i)
        if (auto r = acquisition->persister().save_analysis(numbered("66573", i)); !r) {
          write_error = r.error().what;
          break;
        }
      done = true;
    }));
    while (!done) {
      if (Clock::now() > deadline) {
        read_error = "the writer was still not done after 30 s";
        break;
      }
      if (auto b = reader->browse({}); !b) {
        read_error = b.error().what;
        break;
      }
      ++reads;
    }
  }
  EXPECT_EQ(write_error, "");
  EXPECT_EQ(read_error, "");
  EXPECT_GT(reads, 0);
  auto found = reader->find_analyses({.identifier = "66573"});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->size(), static_cast<std::size_t>(kRuns));
}

TEST_P(AcquisitionTest, ServesManyCallersThenCloses) {
  const auto begin = Clock::now();
  auto acquisition = open();
  ASSERT_TRUE(acquisition);
  std::atomic<int> failures{0};
  {
    std::vector<std::unique_ptr<Joined>> callers;
    callers.reserve(3);
    for (int c = 0; c < 3; ++c)
      callers.push_back(std::make_unique<Joined>(std::thread([&, c] {
        for (int i = 1; i <= 20; ++i)
          if (!acquisition->persister().save_analysis(numbered("66573", c * 20 + i))) ++failures;
      })));
  }
  acquisition.reset();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(analyses_of("66573"), 60U);
  EXPECT_LT(Clock::now() - begin, kBound);
}

// The Qt application a store needs is made by the caller of open_acquisition,
// not by the worker that opens the store.
TEST_P(AcquisitionTest, TheQtApplicationBelongsToTheCallersThread) {
  auto acquisition = open();
  ASSERT_TRUE(acquisition);
  ASSERT_NE(QCoreApplication::instance(), nullptr);
  EXPECT_EQ(QCoreApplication::instance()->thread(), QThread::currentThread());
}

INSTANTIATE_TEST_SUITE_P(Engines, AcquisitionTest, ::testing::ValuesIn(pt::engines()));

TEST(OpenAcquisition, RefusesASchemaThatIsBehind) {
  pt::TestDatabase tdb("sqlite", true);
  const fs::path data = scratch_dir();
  auto acquisition = store::open_acquisition(tdb.url(), data, "test");
  ASSERT_FALSE(acquisition.has_value());
  EXPECT_NE(acquisition.error().what.find("elctl db migrate --db " + tdb.url()), std::string::npos)
      << acquisition.error().what;
  std::error_code ignored;
  fs::remove_all(data, ignored);
}

TEST(OpenAcquisition, AFailureToOpenIsReturned) {
  const fs::path data = scratch_dir();
  const auto begin = std::chrono::steady_clock::now();
  auto acquisition = store::open_acquisition("sqlite:/nonexistent-dir-for-pychron/none.db", data, "test");
  EXPECT_FALSE(acquisition.has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(10));
  std::error_code ignored;
  fs::remove_all(data, ignored);
}

TEST_P(AcquisitionTest, IsOneClientPerHost) {
  auto first = open("a");
  auto second = open("b");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(first->persister().save_analysis(numbered("66573", 1)));
  ASSERT_TRUE(second->persister().save_analysis(numbered("66573", 2)));

  auto reader = pt::open_or_die(tdb_->url());
  ASSERT_TRUE(reader);
  auto page = reader->changes_since(0, 10000);
  ASSERT_TRUE(page.has_value());
  std::set<std::string> clients;
  for (const auto& e : page->entries)
    if (e.kind == "ingest") clients.insert(e.client.str());
  EXPECT_EQ(clients.size(), 1U);
}

// A simulated session's runs go to a database only when it is the install's
// own file on this computer, or when the operator said so.
TEST(SavesSimulatedRuns, OnlyToAnInstallsOwnLocalDatabaseUnlessAsked) {
  using store::saves_simulated_runs;
  // The install's own database (not --db), a file here.
  EXPECT_TRUE(saves_simulated_runs("sqlite:/lab/data/pychron.db", false, false));
  EXPECT_TRUE(saves_simulated_runs("sqlite::memory:", false, false));
  // The install's own, on a server: shared.
  EXPECT_FALSE(saves_simulated_runs("postgresql://jross:pw@db.lab/pychron", false, false));
  EXPECT_FALSE(saves_simulated_runs("postgres://db.lab/pychron", false, false));
  // Given with --db: somebody else's until said otherwise, file or not.
  EXPECT_FALSE(saves_simulated_runs("sqlite:/lab/data/pychron.db", true, false));
  EXPECT_FALSE(saves_simulated_runs("postgresql://db.lab/pychron", true, false));
  // --sim-save.
  EXPECT_TRUE(saves_simulated_runs("postgresql://db.lab/pychron", false, true));
  EXPECT_TRUE(saves_simulated_runs("postgresql://db.lab/pychron", true, true));
  EXPECT_TRUE(saves_simulated_runs("sqlite:/elsewhere.db", true, true));
  // Not a url this program writes to.
  EXPECT_FALSE(saves_simulated_runs("", false, false));
  EXPECT_FALSE(saves_simulated_runs("mysql://db/pychron", false, false));
}

TEST(WithoutPassword, HidesOnlyThePassword) {
  EXPECT_EQ(store::without_password("postgresql://jross:s3cret@db.lab:5432/pychron?sslmode=require"),
            "postgresql://jross@db.lab:5432/pychron?sslmode=require");
  EXPECT_EQ(store::without_password("postgresql://jross@db.lab/pychron"), "postgresql://jross@db.lab/pychron");
  EXPECT_EQ(store::without_password("postgresql://db.lab/pychron"), "postgresql://db.lab/pychron");
  EXPECT_EQ(store::without_password("sqlite:/data/a:b@c.db"), "sqlite:/data/a:b@c.db");
  EXPECT_EQ(store::without_password("sqlite::memory:"), "sqlite::memory:");
}
