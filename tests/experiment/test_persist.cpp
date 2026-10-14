#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "temp_path.hpp"

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

}  // namespace
