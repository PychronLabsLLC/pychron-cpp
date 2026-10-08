// elctl entry: sample and package entry driven through elctl::run against a
// file-backed SQLite store (entry spec, section 10).

#include <gtest/gtest.h>

#include "elctl_fixture.hpp"
#include "entry.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_HAS_STORE

TEST(EntryCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"entry", "samples", "list", "--db", "sqlite::memory:"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
}

#else

#include <string>
#include <vector>

#include "pychron/persistence/store.hpp"

namespace ps = pychron::persistence;

namespace {

class EntryCmd : public elctl::testing::ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    db_ = "sqlite:" + path("store.db").string();
    auto store = ps::open_store(ps::StoreConfig{db_, true});  // elctl entry never migrates
    ASSERT_TRUE(store) << pychron::to_string(store.error());
  }

  Outcome entry(std::vector<std::string> args) const {
    args.insert(args.begin(), "entry");
    args.insert(args.end(), {"--db", db_, "--user", "tester"});
    return run_raw(std::move(args));
  }

  std::string db_;
};

constexpr const char* kSamples =
    "sample,project,pi,material,grainsize,lat,lon\n"
    "FC-2,Irradiation-NM-001,NMGRL Lab,sanidine,,,\n"
    "bt-1,Alpha,\"Ross, J\",biotite,20-40,34.1,-106.9\n"
    "bt-2,Alpha,\"Ross, J\",biotite,20-40,,\n";

}  // namespace

TEST_F(EntryCmd, HelpAndUsage) {
  EXPECT_EQ(run_raw({"entry", "help"}).code, elctl::kOk);
  EXPECT_TRUE(contains(run_raw({"entry", "help"}).out, "identifiers generate"));
  EXPECT_EQ(entry({"samples", "nothing"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"entry", "samples", "list"}).code, elctl::kUsage);  // no --db
}

TEST_F(EntryCmd, SamplesTemplateAndDryRun) {
  ASSERT_EQ(entry({"samples", "template", path("t.csv").string()}).code, elctl::kOk);
  ASSERT_TRUE(std::filesystem::exists(path("t.csv")));

  write("s.csv", kSamples);
  // "NMGRL Lab" is not a PI name until the settings allow it.
  Outcome o = entry({"samples", "import", path("s.csv").string(), "--dry-run"});
  EXPECT_EQ(o.code, elctl::kFailed) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "line 2 error FC-2")) << o.out;
  ASSERT_EQ(entry({"settings", "set", "pi_names_allowed", "[\"NMGRL Lab\"]"}).code, elctl::kOk);
  o = entry({"samples", "import", path("s.csv").string(), "--dry-run"});
  EXPECT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "3 to create")) << o.out;
  EXPECT_EQ(entry({"samples", "list"}).out, "");  // nothing written
}

TEST_F(EntryCmd, ImportThenReimport) {
  ASSERT_EQ(entry({"settings", "set", "pi_names_allowed", "[\"NMGRL Lab\"]"}).code, elctl::kOk);
  write("s.csv", kSamples);
  Outcome o = entry({"samples", "import", path("s.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "new project Alpha (Ross, J)")) << o.out;
  o = entry({"samples", "import", path("s.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "0 to create, 0 differing, 3 stored")) << o.out;
  o = entry({"samples", "list", "--project", "Alpha"});
  EXPECT_TRUE(contains(o.out, "bt-1\tAlpha\tRoss, J\tbiotite\t20-40")) << o.out;
  EXPECT_FALSE(contains(o.out, "FC-2"));
}

TEST_F(EntryCmd, PackagePositionsIdentifiers) {
  ASSERT_EQ(entry({"settings", "set", "pi_names_allowed", "[\"NMGRL Lab\"]"}).code, elctl::kOk);
  write("s.csv", kSamples);
  ASSERT_EQ(entry({"samples", "import", path("s.csv").string()}).code, elctl::kOk);
  write("24-hole.txt", "circle,0.0175\n0,0\n1,0\n2,0\n3,0\n");
  ASSERT_EQ(entry({"holders", "import", path("24-hole.txt").string()}).code, elctl::kOk);

  // A plain package needs no chronology or reactor.
  Outcome o = entry({"package", "add", "P-1", "--kind", "package", "--levels", "A"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  o = entry({"package", "show", "P-1"});
  EXPECT_TRUE(contains(o.out, "P-1 (package)")) << o.out;

  // An irradiation does.
  o = entry({"package", "add", "NM-001", "--levels", "A-B", "--holder", "24-hole"});
  EXPECT_EQ(o.code, elctl::kFailed) << o.out;
  EXPECT_TRUE(contains(o.out, "needs a reactor")) << o.out;
  write("chron.txt", "1.0,2026-09-01 08:00:00,2026-09-01 16:00:00\n");
  o = entry({"package", "add", "NM-001", "--levels", "A-B", "--holder", "24-hole", "--z", "0.5", "--reactor", "Triga",
             "--chronology", path("chron.txt").string(), "--tz", "America/Denver"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "8 h")) << o.out;

  write("p.csv",
        "level,position,sample,project,principal_investigator,material,grainsize,packet\n"
        "A,1,FC-2,Irradiation-NM-001,NMGRL Lab,sanidine,,P1\n"
        "A,3,bt-1,Alpha,\"Ross, J\",biotite,20-40,P2\n"
        "B,2,bt-2,,,,,\n");
  o = entry({"positions", "import", "NM-001", path("p.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;

  o = entry({"identifiers", "generate", "NM-001", "--dry-run"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "A1 FC-2 - -> 1\nA3 bt-1 - -> 2\nB2 bt-2 - -> 3\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "warning level B has no monitor")) << o.out;
  o = entry({"identifiers", "generate", "NM-001"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  o = entry({"package", "show", "NM-001", "--csv"});
  EXPECT_TRUE(contains(o.out, "A,1,1,FC-2,")) << o.out;
  EXPECT_TRUE(contains(o.out, "B,2,3,bt-2,")) << o.out;
  // Numbered positions are not numbered again.
  o = entry({"identifiers", "generate", "NM-001"});
  EXPECT_TRUE(contains(o.out, "0 identifiers")) << o.out;

  ASSERT_EQ(entry({"package", "set-kind", "NM-001", "package"}).code, elctl::kOk);
  EXPECT_TRUE(contains(entry({"package", "show", "NM-001"}).out, "NM-001 (package)"));
}

TEST_F(EntryCmd, PositionsImportErrorsWriteNothing) {
  ASSERT_EQ(entry({"package", "add", "P-1", "--kind", "package", "--levels", "A"}).code, elctl::kOk);
  write("p.csv", "level,position,sample\nA,1,nope\nZ,1,x\n");
  const Outcome o = entry({"positions", "import", "P-1", path("p.csv").string()});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.out, "line 2: no sample nope")) << o.out;
  EXPECT_TRUE(contains(o.out, "line 3: no level 'Z'")) << o.out;
}

#endif

TEST_F(EntryCmd, SeedAppliesAFileAndDryRunWritesNothing) {
  const auto file = path("seed.toml");
  {
    std::ofstream out(file);
    out << "project = \"references\"\n[[samples]]\nidentifier = \"a\"\nanalysis_type = \"air\"\n"
           "sample = \"air\"\nmaterial = \"air\"\n[reactors.Triga]\nK4039 = [0.00873, 0.00017]\n";
  }
  auto o = entry({"seed", file.string(), "--dry-run"});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "would be seeded 1 project, 1 material, 1 sample, 1 identifier, 1 reactor")) << o.out;
  EXPECT_EQ(entry({"samples", "list", "--project", "references"}).out, "");

  o = entry({"seed", file.string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "seeded 1 project, 1 material, 1 sample, 1 identifier, 1 reactor")) << o.out;
  EXPECT_TRUE(contains(entry({"samples", "list", "--project", "references"}).out, "air\treferences"));

  o = entry({"seed", file.string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "seed: nothing to add (5 already there)")) << o.out;

  EXPECT_EQ(entry({"seed", path("missing.toml").string()}).code, 2);
  EXPECT_EQ(entry({"seed"}).code, 2);
  EXPECT_EQ(entry({"seed", file.string(), "extra"}).code, 2);
  { std::ofstream(file) << "project = \n"; }
  o = entry({"seed", file.string()});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "seed.toml")) << o.err;
}
