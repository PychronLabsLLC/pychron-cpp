#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "log_capture.hpp"
#include "temp_path.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::persist;
namespace fs = std::filesystem;

namespace {

record::AnalysisRecord rec(const std::string& uuid, const std::string& identifier = "12345", int aliquot = 1,
                           const std::string& step = "") {
  record::AnalysisRecord r;
  r.identity.uuid = uuid;
  r.identity.identifier = identifier;
  r.identity.aliquot = aliquot;
  r.identity.step = step;
  r.identity.analysis_type = "unknown";
  return r;
}

class FlakyPersister final : public IAnalysisPersister {
 public:
  Result<int> next_aliquot(const std::string&) override { return 1; }
  Result<void> begin_run(const RunIdentity&, const QueueSpec&) override { return {}; }
  Result<void> save_extraction(const record::AnalysisRecord&) override { return {}; }
  Result<void> save_analysis(const record::AnalysisRecord& r) override {
    if (down) return fail(ErrorKind::Io, "database down");
    saved.push_back(r.identity.uuid);
    return {};
  }
  Result<void> save_artifact(const std::string&, const std::string&, const std::vector<std::uint8_t>&) override {
    return {};
  }
  Result<void> flush() override { return {}; }
  Result<std::vector<std::string>> check_queue(const QueueSpec&) override { return std::vector<std::string>{}; }
  bool down = false;
  std::vector<std::string> saved;
};

class PersistTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = pychron::testing::unique_temp_path("persist_");
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  // An empty file <dir_>/records/12345/<name>.
  void touch(const std::string& name) const {
    const fs::path dir = dir_ / "records" / "12345";
    fs::create_directories(dir);
    std::ofstream(dir / name).flush();
  }
  fs::path dir_;
};

TEST_F(PersistTest, FilePersisterWritesRecordsAndCountsAliquots) {
  FilePersister p(dir_ / "records");
  EXPECT_EQ(*p.next_aliquot("12345"), 1);
  EXPECT_EQ(*p.next_aliquot("12345"), 2);  // handed out this session
  ASSERT_TRUE(p.begin_run(RunIdentity{"12345", 5, "A"}, QueueSpec{}));
  ASSERT_TRUE(p.save_analysis(rec("u1", "12345", 5, "A")));
  ASSERT_TRUE(p.save_extraction(rec("u1", "12345", 5, "A")));
  EXPECT_TRUE(fs::exists(dir_ / "records" / "12345" / "12345-5A.json"));
  EXPECT_TRUE(fs::exists(dir_ / "records" / "12345" / "12345-5A.extraction.json"));
  auto back = record::from_json([&] {
    std::ifstream in(dir_ / "records" / "12345" / "12345-5A.json");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }());
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(back->identity.uuid, "u1");

  // A new session sees what is on disk.
  FilePersister fresh(dir_ / "records");
  EXPECT_EQ(*fresh.next_aliquot("12345"), 6);
  EXPECT_EQ(*fresh.next_aliquot("99999"), 1);
  EXPECT_FALSE(fresh.next_aliquot("../etc"));
  ASSERT_TRUE(fresh.save_artifact("u1", "snap.png", {1, 2, 3}));
  EXPECT_EQ(fs::file_size(dir_ / "records" / "artifacts" / "u1" / "snap.png"), 3u);
  EXPECT_FALSE(fresh.save_artifact("u1", "../x", {}));
}

TEST_F(PersistTest, FilePersisterLastRunIsTheHighestAliquotThenItsHighestStep) {
  touch("12345-3A.json");
  touch("12345-3B.json");
  touch("12345-2.json");
  {
    const fs::path other = dir_ / "records" / "99999";  // another identifier
    fs::create_directories(other);
    std::ofstream(other / "99999-7C.json").flush();
  }
  FilePersister files(dir_ / "records");
  auto last = files.last_run("12345");
  ASSERT_TRUE(last) << last.error().what;
  ASSERT_TRUE(last->has_value());
  EXPECT_EQ(**last, (LastRun{3, 1}));
}

TEST_F(PersistTest, FilePersisterLastRunIsNoneWithoutFiles) {
  FilePersister files(dir_ / "records");
  auto last = files.last_run("12345");
  ASSERT_TRUE(last) << last.error().what;
  EXPECT_FALSE(last->has_value());
}

TEST_F(PersistTest, FilePersisterLastRunOfANameWithNoStepHasStepMinusOne) {
  touch("12345-2B.json");
  touch("12345-3.json");
  FilePersister files(dir_ / "records");
  EXPECT_EQ(files.last_run("12345").value(), (LastRun{3, -1}));
}

TEST_F(PersistTest, FilePersisterLastRunCountsAnExtractionFile) {
  touch("12345-3A.json");
  touch("12345-3B.extraction.json");
  FilePersister files(dir_ / "records");
  EXPECT_EQ(files.last_run("12345").value(), (LastRun{3, 1}));
}

TEST_F(PersistTest, FilePersisterLastRunIgnoresNamesThatAreNotRuns) {
  touch("12345-3A.json");
  touch("12345-9.txt");
  touch("12345-.json");
  touch("12345-8B_x.json");
  touch("12345-7A.json.tmp");
  FilePersister files(dir_ / "records");
  EXPECT_EQ(files.last_run("12345").value(), (LastRun{3, 0}));
}

TEST_F(PersistTest, FilePersisterLastRunRefusesAnIdentifierThatIsNotAPlainName) {
  FilePersister files(dir_ / "records");
  EXPECT_FALSE(files.last_run("../x"));
}

TEST_F(PersistTest, FilePersisterLastRunIssuesNothing) {
  touch("12345-3B.json");
  FilePersister files(dir_ / "records");
  (void)files.last_run("12345");
  EXPECT_EQ(files.next_aliquot("12345").value(), 4);
}

TEST_F(PersistTest, DefaultLastRunIsNone) {
  FlakyPersister flaky;
  auto last = flaky.last_run("12345");
  ASSERT_TRUE(last) << last.error().what;
  EXPECT_FALSE(last->has_value());
}

TEST_F(PersistTest, FilePersisterChecksNothing) {
  FilePersister files(dir_ / "records");
  QueueSpec queue;
  queue.runs.emplace_back();
  queue.runs.back().id.identifier = "zz";
  auto problems = files.check_queue(queue);
  ASSERT_TRUE(problems) << problems.error().what;
  EXPECT_TRUE(problems->empty());
  auto none = files.check_queue(QueueSpec{});
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->empty());
}

TEST_F(PersistTest, SpoolRoundTripsAndListsOldestFirst) {
  Spool spool(dir_ / "spool");
  EXPECT_TRUE(spool.pending()->empty());  // no directory yet
  ASSERT_TRUE(spool.put(rec("a")));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(spool.put(rec("b")));
  EXPECT_EQ(*spool.pending(), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(spool.get("b")->identity.uuid, "b");
  ASSERT_TRUE(spool.remove("a"));
  EXPECT_EQ(*spool.pending(), std::vector<std::string>{"b"});
  EXPECT_FALSE(spool.put(rec("")));  // a record needs a uuid
  EXPECT_FALSE(fs::exists(dir_ / "spool" / "b.json.tmp"));
}

TEST_F(PersistTest, SavePipelineSpoolsFirstAndRetries) {
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  SavePipeline pipe(spool, db);
  ASSERT_TRUE(pipe.save(rec("a")));
  EXPECT_EQ(db.saved, std::vector<std::string>{"a"});
  EXPECT_EQ(pipe.pending(), 0u);

  db.down = true;
  ASSERT_TRUE(pipe.save(rec("b")));  // persister failure does not fail the save
  EXPECT_EQ(pipe.pending(), 1u);
  ASSERT_TRUE(pipe.last_error());
  EXPECT_NE(pipe.last_error()->find("database down"), std::string::npos);
  EXPECT_FALSE(pipe.flush());  // still down: nothing persisted

  db.down = false;
  auto n = pipe.flush();
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 1u);
  EXPECT_EQ(db.saved, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(pipe.pending(), 0u);
}

// The caller of save() is told when the record got no further than the spool,
// and why; the pipeline keeps the reason until that record has been sent.
TEST_F(PersistTest, AHandOffThatFailsIsReportedAndClearedOnceTheRecordIsSent) {
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  SavePipeline pipe(spool, db);
  auto sent = pipe.save(rec("a"));
  ASSERT_TRUE(sent);
  EXPECT_FALSE(sent->pending.has_value());
  EXPECT_FALSE(pipe.last_error().has_value());

  db.down = true;
  auto kept = pipe.save(rec("b"));
  ASSERT_TRUE(kept);  // spooled: not lost
  ASSERT_TRUE(kept->pending.has_value());
  EXPECT_EQ(kept->pending->kind, ErrorKind::Io);
  EXPECT_EQ(kept->pending->what, "database down");
  EXPECT_EQ(pipe.pending(), 1u);
  ASSERT_TRUE(pipe.last_error().has_value());
  EXPECT_EQ(*pipe.last_error(), "b: database down");

  db.down = false;
  auto n = pipe.recover();
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 1u);
  EXPECT_EQ(pipe.pending(), 0u);
  EXPECT_FALSE(pipe.last_error().has_value());  // nothing is waiting: nothing to explain
}

// A record that cannot be spooled is the one failure of save().
TEST_F(PersistTest, ARecordThatCannotBeSpooledFailsTheSave) {
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  SavePipeline pipe(spool, db);
  EXPECT_FALSE(pipe.save(rec("")));
  EXPECT_TRUE(db.saved.empty());
}

// With a post function the hand-off happens later: save() cannot say how it went.
TEST_F(PersistTest, APostedHandOffIsNotReportedBySave) {
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  db.down = true;
  std::vector<std::function<void()>> posted;
  SavePipeline pipe(spool, db, [&](std::function<void()> f) { posted.push_back(std::move(f)); });
  auto r = pipe.save(rec("a"));
  ASSERT_TRUE(r);
  EXPECT_FALSE(r->pending.has_value());
  ASSERT_EQ(posted.size(), 1u);
  posted.front()();
  ASSERT_TRUE(pipe.last_error().has_value());
  EXPECT_EQ(pipe.pending(), 1u);
}

// What is in the spool folder and is not a record file is not a pending
// record, and does not stop the listing: a folder, and a name whose file has
// gone (here a link to nothing, which is what a file removed between the
// listing and the look at its time is to the second call).
TEST_F(PersistTest, TheSpoolListsRecordFilesOnlyAndSurvivesOneThatVanished) {
  Spool spool(dir_ / "spool");
  ASSERT_TRUE(spool.put(rec("a")));
  fs::create_directories(dir_ / "spool" / "folder.json");
  std::error_code ec;
  fs::create_symlink(dir_ / "spool" / "nowhere", dir_ / "spool" / "gone.json", ec);
  const bool linked = !ec;  // not every file system or account may make one
  auto listed = spool.pending();
  ASSERT_TRUE(listed) << listed.error().what;
  EXPECT_EQ(*listed, std::vector<std::string>{"a"});
  SUCCEED() << (linked ? "with a dangling link" : "without a link");
}

TEST_F(PersistTest, RecoverResendsAnEarlierSessionsSpool) {
  {
    Spool spool(dir_ / "spool");
    FlakyPersister db;
    db.down = true;
    SavePipeline pipe(spool, db);
    ASSERT_TRUE(pipe.save(rec("left-behind")));
  }
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  SavePipeline pipe(spool, db);
  EXPECT_EQ(*pipe.recover(), 1u);
  EXPECT_EQ(db.saved, std::vector<std::string>{"left-behind"});
}

TEST_F(PersistTest, SavePipelinePostsTheHandOff) {
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  std::vector<std::function<void()>> queued;
  SavePipeline pipe(spool, db, [&](std::function<void()> f) { queued.push_back(std::move(f)); });
  ASSERT_TRUE(pipe.save(rec("a")));
  EXPECT_TRUE(db.saved.empty());  // not yet run
  EXPECT_EQ(pipe.pending(), 1u);
  ASSERT_EQ(queued.size(), 1u);
  queued[0]();
  EXPECT_EQ(db.saved, std::vector<std::string>{"a"});
  EXPECT_EQ(pipe.pending(), 0u);
}

TEST_F(PersistTest, AliquotAllocator) {
  FlakyPersister db;
  AliquotAllocator alloc(db);
  auto a = alloc.allocate(RunIdentity{"12345", std::nullopt, "B"});
  ASSERT_TRUE(a);
  EXPECT_EQ(a->aliquot, 1);
  EXPECT_EQ(a->step, "B");
  EXPECT_EQ(alloc.allocate(RunIdentity{"12345", 7, ""})->aliquot, 7);  // user-fixed
  EXPECT_FALSE(alloc.allocate(RunIdentity{"12345", 0, ""}));
}

// A step after the last one given to its identifier takes that aliquot, so a
// step heat is one aliquot. A new heating (its A, or a step not after the last
// one when its A was skipped) and a run with no step take the next free one.
TEST_F(PersistTest, AliquotAllocatorKeepsTheStepsOfAHeatingTogether) {
  FilePersister files(dir_ / "records");
  AliquotAllocator alloc(files);
  struct Row {
    std::string identifier;
    std::optional<int> aliquot;
    std::string step;
    int expected;
  };
  const std::vector<Row> rows{
      {"12345", std::nullopt, "A", 1},  // a heating
      {"12345", std::nullopt, "B", 1},
      {"99999", std::nullopt, "A", 1},  // another identifier between its steps
      {"12345", std::nullopt, "", 2},   // a run with no step: its own aliquot, the heating remembered
      {"12345", std::nullopt, "C", 1},
      {"99999", std::nullopt, "B", 1},
      {"12345", std::nullopt, "A", 3},  // the next heating
      {"12345", std::nullopt, "C", 3},  // its B skipped
      {"12345", std::nullopt, "B", 4},  // a heating whose A was skipped: never 3, whose B exists
      {"12345", 7, "A", 7},             // a fixed aliquot starts a heating too
      {"12345", std::nullopt, "B", 7},
  };
  for (const auto& r : rows) {
    auto a = alloc.allocate(RunIdentity{r.identifier, r.aliquot, r.step});
    ASSERT_TRUE(a) << r.identifier << r.step;
    EXPECT_EQ(a->aliquot, r.expected) << r.identifier << " step '" << r.step << "'";
  }
}

// A persister whose last saved run of every identifier is `last` (3B unless
// a test changes it); `down` makes last_run fail. Next free aliquot 4.
class SavedRunPersister final : public IAnalysisPersister {
 public:
  Result<int> next_aliquot(const std::string&) override { return next++; }
  Result<std::optional<LastRun>> last_run(const std::string&) override {
    ++asked;
    if (down) return fail(ErrorKind::Io, "database down");
    return last;
  }
  Result<void> begin_run(const RunIdentity&, const QueueSpec&) override { return {}; }
  Result<void> save_extraction(const record::AnalysisRecord&) override { return {}; }
  Result<void> save_analysis(const record::AnalysisRecord&) override { return {}; }
  Result<void> save_artifact(const std::string&, const std::string&, const std::vector<std::uint8_t>&) override {
    return {};
  }
  Result<void> flush() override { return {}; }
  Result<std::vector<std::string>> check_queue(const QueueSpec&) override { return std::vector<std::string>{}; }
  std::optional<LastRun> last = LastRun{3, 1};
  bool down = false;
  int asked = 0;
  int next = 4;
};

// Each row a fresh allocator over a persister whose last saved run is 3B.
TEST_F(PersistTest, AliquotAllocatorJoinsTheLastSavedHeating) {
  struct Row {
    std::string step;
    int expected;
  };
  for (const Row& r : std::vector<Row>{{"C", 3}, {"D", 3}, {"B", 4}, {"A", 4}, {"", 4}}) {
    SavedRunPersister db;
    AliquotAllocator alloc(db);
    auto a = alloc.allocate(RunIdentity{"12345", std::nullopt, r.step});
    ASSERT_TRUE(a) << r.step;
    EXPECT_EQ(a->aliquot, r.expected) << "step '" << r.step << "'";
  }
}

TEST_F(PersistTest, AliquotAllocatorDoesNotJoinWhenTheNewestAliquotHasNoStep) {
  SavedRunPersister db;
  db.last = LastRun{3, -1};
  AliquotAllocator alloc(db);
  EXPECT_EQ(alloc.allocate(RunIdentity{"12345", std::nullopt, "B"})->aliquot, 4);
}

TEST_F(PersistTest, AliquotAllocatorWithNothingSavedTakesTheNextAliquot) {
  SavedRunPersister db;
  db.last.reset();
  AliquotAllocator alloc(db);
  EXPECT_EQ(alloc.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 4);
}

TEST_F(PersistTest, AliquotAllocatorFailsWhenLastRunFails) {
  SavedRunPersister db;
  db.down = true;
  AliquotAllocator alloc(db);
  auto a = alloc.allocate(RunIdentity{"12345", std::nullopt, "C"});
  ASSERT_FALSE(a);
  EXPECT_EQ(a.error().what, "database down");
  EXPECT_EQ(db.next, 4);  // no aliquot was issued
}

// A failed last_run is not an answer: the retry asks again and joins 3B.
TEST_F(PersistTest, AliquotAllocatorAsksAgainAfterLastRunFailed) {
  SavedRunPersister db;
  db.down = true;
  AliquotAllocator alloc(db);
  ASSERT_FALSE(alloc.allocate(RunIdentity{"12345", std::nullopt, "C"}));
  db.down = false;
  auto a = alloc.allocate(RunIdentity{"12345", std::nullopt, "C"});
  ASSERT_TRUE(a);
  EXPECT_EQ(a->aliquot, 3);
  EXPECT_EQ(db.asked, 2);
  EXPECT_EQ(db.next, 4);  // no aliquot was issued
}

TEST_F(PersistTest, AliquotAllocatorAsksOncePerIdentifierAndNeverForAOrNoStep) {
  SavedRunPersister db;
  AliquotAllocator alloc(db);
  ASSERT_TRUE(alloc.allocate(RunIdentity{"12345", std::nullopt, "A"}));  // 4A: not asked
  ASSERT_TRUE(alloc.allocate(RunIdentity{"12345", std::nullopt, ""}));   // not asked
  EXPECT_EQ(db.asked, 0);
  SavedRunPersister db2;
  AliquotAllocator alloc2(db2);
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 3);
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "D"})->aliquot, 3);  // memory answers
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "B"})->aliquot, 4);  // not after D: T090
  EXPECT_EQ(alloc2.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 4);
  EXPECT_EQ(db2.asked, 1);
}

TEST_F(PersistTest, AnAllocatorOverTheSameFilesContinuesAHeatingAfterARestart) {
  FilePersister files(dir_ / "records");
  {
    AliquotAllocator before(files);
    ASSERT_EQ(before.allocate(RunIdentity{"12345", std::nullopt, "A"})->aliquot, 1);
  }
  touch("12345-1A.json");  // what the first session saved
  FilePersister restarted(dir_ / "records");
  AliquotAllocator after(restarted);
  EXPECT_EQ(after.allocate(RunIdentity{"12345", std::nullopt, "B"})->aliquot, 1);
  EXPECT_EQ(after.allocate(RunIdentity{"12345", std::nullopt, "C"})->aliquot, 1);
  EXPECT_EQ(after.allocate(RunIdentity{"12345", std::nullopt, "A"})->aliquot, 2);
}

// ---- SpoolFlusher (executor hardening spec 4.8) ----------------------------------

using namespace std::chrono_literals;

// A persister that refuses the records named in `refused` (all of them while
// `down`), and notes on the clock when it is asked to flush, which the
// pipeline does once per flush. Called from the scheduler's worker.
class ClockedPersister final : public IAnalysisPersister {
 public:
  explicit ClockedPersister(const Clock& clock) : clock_(clock) {}
  Result<int> next_aliquot(const std::string&) override { return 1; }
  Result<void> begin_run(const RunIdentity&, const QueueSpec&) override { return {}; }
  Result<void> save_extraction(const record::AnalysisRecord&) override { return {}; }
  Result<void> save_analysis(const record::AnalysisRecord& r) override {
    const std::lock_guard lock(mutex_);
    if (down_ || refused_.contains(r.identity.uuid)) return fail(ErrorKind::Io, "database down");
    return {};
  }
  Result<void> save_artifact(const std::string&, const std::string&, const std::vector<std::uint8_t>&) override {
    return {};
  }
  Result<void> flush() override {
    const std::lock_guard lock(mutex_);
    flushes_.push_back(clock_.now());
    return {};
  }
  Result<std::vector<std::string>> check_queue(const QueueSpec&) override { return std::vector<std::string>{}; }

  void set_down(bool down) {
    const std::lock_guard lock(mutex_);
    down_ = down;
  }
  void refuse(std::set<std::string> uuids) {
    const std::lock_guard lock(mutex_);
    refused_ = std::move(uuids);
  }
  // When each flush came, as offsets from `t0`.
  std::vector<pychron::Duration> flushes_since(TimePoint t0) const {
    const std::lock_guard lock(mutex_);
    std::vector<pychron::Duration> out;
    out.reserve(flushes_.size());
    for (const auto t : flushes_) out.push_back(t - t0);
    return out;
  }

 private:
  const Clock& clock_;
  mutable std::mutex mutex_;
  bool down_ = false;
  std::set<std::string> refused_;
  std::vector<TimePoint> flushes_;
};

class SpoolFlusherTest : public pychron::testing::VirtualTimeTest {
 protected:
  void SetUp() override {
    dir_ = pychron::testing::unique_temp_path("spool_flush_");
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path dir_;
};

// What a flusher works on, declared in the order it is torn down safely: the
// flusher before the scheduler, the scheduler before what its jobs touch.
struct FlushWorld {
  explicit FlushWorld(const fs::path& dir) : spool(dir / "spool") {
    subs.push_back(bus.subscribe<SpoolStatus>([this](const SpoolStatus& s) {
      const std::lock_guard lock(mutex);
      statuses.push_back(s);
    }));
    scheduler.start();
  }
  std::vector<SpoolStatus> published() {
    const std::lock_guard lock(mutex);
    return statuses;
  }

  VirtualClock clock;
  Clock::Participant me{clock, "test"};
  SignalBus bus;
  std::mutex mutex;
  std::vector<SpoolStatus> statuses;
  std::vector<SignalBus::Subscription> subs;
  ClockedPersister db{clock};
  Spool spool;
  SavePipeline pipe{spool, db};
  Scheduler scheduler{clock, &bus, Scheduler::Options{1}};
  SpoolFlusher flusher{pipe, scheduler, clock, &bus};
};

TEST_F(SpoolFlusherTest, AFlushRunsAfterEveryRunAndNothingIsLeftScheduledWhenNothingIsPending) {
  FlushWorld w(dir_);
  const TimePoint t0 = w.clock.now();
  w.flusher.flush_now();  // a run has finished
  w.clock.sleep_for(10min);
  w.flusher.flush_now();  // and another
  w.clock.sleep_for(1h);
  EXPECT_EQ(w.db.flushes_since(t0), (std::vector<pychron::Duration>{0min, 10min}));
  EXPECT_EQ(w.scheduler.job_count(), 0u);
  EXPECT_EQ(w.published(), std::vector<SpoolStatus>{});  // nothing pending, as it was
}

TEST_F(SpoolFlusherTest, WhileARecordIsPendingItRetriesEveryFiveMinutesBackingOffToThirty) {
  FlushWorld w(dir_);
  ASSERT_TRUE(w.spool.put(rec("u1")));
  w.db.set_down(true);
  const TimePoint t0 = w.clock.now();
  w.flusher.flush_now();
  w.clock.sleep_for(100min);
  EXPECT_EQ(w.db.flushes_since(t0), (std::vector<pychron::Duration>{0min, 5min, 15min, 35min, 65min, 95min}));
  w.db.set_down(false);
  w.clock.sleep_for(1h);  // the flush at 125 min sends it, and none follows
  EXPECT_EQ(w.db.flushes_since(t0), (std::vector<pychron::Duration>{0min, 5min, 15min, 35min, 65min, 95min, 125min}));
  EXPECT_EQ(w.pipe.pending(), 0u);
  EXPECT_EQ(w.scheduler.job_count(), 0u);
}

TEST_F(SpoolFlusherTest, AFlushThatSendsSomethingBringsTheWaitBackToFiveMinutes) {
  FlushWorld w(dir_);
  ASSERT_TRUE(w.spool.put(rec("u1")));
  ASSERT_TRUE(w.spool.put(rec("u2")));
  w.db.set_down(true);
  const TimePoint t0 = w.clock.now();
  w.flusher.flush_now();
  w.clock.sleep_for(20min);  // flushes at 0, 5 and 15 min failed
  w.db.set_down(false);
  w.db.refuse({"u2"});  // u1 goes at 35 min; u2 still does not
  w.clock.sleep_for(40min);
  EXPECT_EQ(w.db.flushes_since(t0), (std::vector<pychron::Duration>{0min, 5min, 15min, 35min, 40min, 45min, 55min}));
  EXPECT_EQ(w.pipe.pending(), 1u);
}

TEST_F(SpoolFlusherTest, ItsStatusIsPublishedOnEachChange) {
  FlushWorld w(dir_);
  ASSERT_TRUE(w.spool.put(rec("u1")));
  w.db.set_down(true);
  w.flusher.flush_now();
  w.clock.sleep_for(1h);  // several failed flushes, one status
  const SpoolStatus failing{1, "u1: database down"};
  EXPECT_EQ(w.published(), std::vector<SpoolStatus>{failing});
  EXPECT_EQ(w.flusher.status(), failing);
  w.db.set_down(false);
  w.clock.sleep_for(1h);
  EXPECT_EQ(w.published(), (std::vector<SpoolStatus>{failing, SpoolStatus{}}));
  EXPECT_EQ(w.flusher.status(), SpoolStatus{});
}

TEST_F(SpoolFlusherTest, AFlushAskedForWhileATimedOneWaitsReplacesIt) {
  FlushWorld w(dir_);
  ASSERT_TRUE(w.spool.put(rec("u1")));
  w.db.set_down(true);
  const TimePoint t0 = w.clock.now();
  w.flusher.flush_now();
  w.clock.sleep_for(2min);
  w.flusher.flush_now();  // the run after: the timed flush at 5 min is not also kept
  // It failed too: the back-off goes on from it (5, then 10 min).
  w.clock.sleep_for(11min);
  EXPECT_EQ(w.db.flushes_since(t0), (std::vector<pychron::Duration>{0min, 2min, 12min}));
  EXPECT_EQ(w.scheduler.job_count(), 1u);
}

}  // namespace

// ---- the log ----------------------------------------------------------------------

TEST_F(PersistTest, SpoolLogSaysWhatIsSpooledAndHandedOver) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  SavePipeline pipe(spool, db);
  ASSERT_TRUE(pipe.save(rec("a")));
  const auto lines = log.lines();
  const auto has = [&](const std::string& prefix) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.starts_with(prefix); });
  };
  EXPECT_TRUE(has("[debug] spool: put 12345-1 (a)")) << ::testing::PrintToString(lines);
  EXPECT_TRUE(has("[debug] spool: hand-off a: sent")) << ::testing::PrintToString(lines);
  EXPECT_TRUE(has("[debug] spool: remove a")) << ::testing::PrintToString(lines);
}

TEST_F(PersistTest, SpoolLogAHandOffThatFailsIsAWarning) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  Spool spool(dir_ / "spool");
  FlakyPersister db;
  db.down = true;
  SavePipeline pipe(spool, db);
  ASSERT_TRUE(pipe.save(rec("b")));
  const auto lines = log.lines();
  EXPECT_NE(std::find(lines.begin(), lines.end(), "[warn] spool: hand-off b: io: database down"), lines.end())
      << ::testing::PrintToString(lines);
}

TEST_F(SpoolFlusherTest, SpoolLogRetryResult) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  FlushWorld w(dir_);
  ASSERT_TRUE(w.spool.put(rec("u1")));
  w.db.set_down(true);
  w.flusher.flush_now();
  w.clock.sleep_for(1min);
  w.db.set_down(false);
  w.clock.sleep_for(10min);  // the retry at 5 min sends it
  const auto lines = log.lines();
  const auto has = [&](const std::string& line) { return std::find(lines.begin(), lines.end(), line) != lines.end(); };
  // The failed hand-off said why once; the flush adds no second warning for it.
  EXPECT_EQ(std::count_if(lines.begin(), lines.end(), [](const std::string& l) { return l.starts_with("[warn] spool:"); }), 1)
      << ::testing::PrintToString(lines);
  EXPECT_TRUE(has("[warn] spool: hand-off u1: io: database down")) << ::testing::PrintToString(lines);
  EXPECT_TRUE(has("[debug] spool: flush: sent 1, 0 pending")) << ::testing::PrintToString(lines);
}
