#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/serialize.hpp"

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
  bool down = false;
  std::vector<std::string> saved;
};

class PersistTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("persist_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

}  // namespace
