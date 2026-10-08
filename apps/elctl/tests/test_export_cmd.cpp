// elctl export: the Schaen et al. (2021) report from a file-backed SQLite
// store, driven through elctl::run.

#include <gtest/gtest.h>

#include "elctl_fixture.hpp"
#include "export.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_HAS_STORE

TEST(ExportCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"export", "--db", "sqlite::memory:", "--out", "x.csv"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
  EXPECT_EQ(o.out, "");
}

#else

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "store_fixture.hpp"

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The seeded store of store_fixture.hpp.
class ExportCmd : public elctl::testing::StoreTest {
 protected:
  Outcome export_(std::vector<std::string> args) const {
    args.insert(args.begin(), {"export", "--db", db_});
    return run_raw(std::move(args));
  }
};

TEST_F(ExportCmd, HelpAndUsageErrors) {
  const Outcome help = run_raw({"export", "help"});
  EXPECT_EQ(help.code, elctl::kOk);
  EXPECT_TRUE(contains(help.out, "Schaen")) << help.out;

  EXPECT_EQ(run_raw({"export", "--out", "x.csv"}).code, elctl::kUsage);
  const Outcome no_out = run_raw({"export", "--db", db_});
  EXPECT_EQ(no_out.code, elctl::kUsage);
  EXPECT_TRUE(contains(no_out.err, "--out")) << no_out.err;
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--sigma", "3"}).err, "--sigma"));
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--from", "yesterday"}).err, "--from"));
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--frob", "1"}).err, "--frob"));
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--group-by", "colour"}).err, "--group-by"));
  EXPECT_EQ(export_({"--out", "x.csv", "--sample"}).code, elctl::kUsage);
  // A number too large to hold is refused, not wrapped into one that fits.
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--limit", "99999999999"}).err, "--limit"));
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--plateau-steps", "4294967299"}).err, "--plateau-steps"));
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--from", "2020-4294967297-01"}).err, "--from"));
  EXPECT_TRUE(contains(export_({"--out", "x.csv", "--plateau-gas", "1e999"}).err, "--plateau-gas"));

  const Outcome missing = run_raw({"export", "--db", "sqlite:" + path("nowhere.db").string(), "--out", "x.csv"});
  EXPECT_EQ(missing.code, elctl::kUsage);
  EXPECT_TRUE(contains(missing.err, "no database at")) << missing.err;
}

TEST_F(ExportCmd, WritesTheCsvReportForTheUnknowns) {
  const fs::path out = path("report.csv");
  const Outcome o = export_({"--out", out.string(), "--lab", "NMGRL", "--note", "test run"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "2 analyses, 1 group (grouped by aliquot)")) << o.out;
  ASSERT_TRUE(fs::is_regular_file(out));
  const std::string csv = slurp(out);
  EXPECT_TRUE(contains(csv, "# 40Ar/39Ar data report after Schaen")) << csv.substr(0, 200);
  for (const char* section : {"[metadata]", "[constants]", "[irradiation]", "[analyses]", "[summary]"})
    EXPECT_TRUE(contains(csv, section)) << section;
  EXPECT_TRUE(contains(csv, "laboratory,NMGRL"));
  EXPECT_TRUE(contains(csv, "note,test run"));
  EXPECT_TRUE(contains(csv, "77000-01A"));
  EXPECT_TRUE(contains(csv, "77000-01B"));
  EXPECT_FALSE(contains(csv, "66574"));  // the air is not an unknown
  // The catalog's sample row, the flux monitor and the reactor.
  EXPECT_TRUE(contains(csv, "77000,FC-2,sanidine,Fish Canyon,\"Ross, J\",37.75,-106.9,2850,ash-flow tuff,,,IEFC20001,NM-300,A,3,Triga,"))
      << csv;
  EXPECT_TRUE(contains(csv, "FC-2,sanidine,28.201,0.046,0.001,2e-06"));
  // Two heating steps of one aliquot: a step-heated group with an integrated age.
  EXPECT_TRUE(contains(csv, "77000-1,FC-2,77000,sanidine,2,2,yes,"));
}

TEST_F(ExportCmd, JsonByExtensionAndFiltersThatMatchNothing) {
  const fs::path out = path("report.json");
  const Outcome o = export_({"--out", out.string(), "--type", "air", "--group-by", "identifier", "--sigma", "1"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "1 analyses, 1 group (grouped by identifier)")) << o.out;
  const std::string json = slurp(out);
  EXPECT_EQ(json.substr(0, 1), "{");
  EXPECT_TRUE(contains(json, "\"run id\": \"66574-01\""));
  EXPECT_TRUE(contains(json, "\"40Ar (fA) ±(1s)\""));

  const Outcome none = export_({"--out", path("none.csv").string(), "--sample", "no-such-sample"});
  EXPECT_EQ(none.code, elctl::kFailed);
  EXPECT_TRUE(contains(none.err, "no analysis matched")) << none.err;
  EXPECT_FALSE(fs::exists(path("none.csv")));

  const Outcome dated = export_({"--out", path("dated.csv").string(), "--from", "2026-10-03"});
  EXPECT_EQ(dated.code, elctl::kFailed);
  const Outcome in_range = export_({"--out", path("in.csv").string(), "--from", "2026-10-02", "--to", "2026-10-02"});
  EXPECT_EQ(in_range.code, elctl::kOk) << in_range.err;
}

// The selection, reduction and plateau flags are parsed by selection.cpp, for
// `earthbank` as well: what each says when it is wrong, and that the first
// wrong word from the left is the one reported, whoever parses it.
TEST_F(ExportCmd, TheSharedFlagsSayWhatTheySaid) {
  const std::string head = "elctl export: ";
  const auto first_line = [](const std::string& text) { return text.substr(0, text.find('\n')); };
  const auto says = [&](std::vector<std::string> args) {
    args.insert(args.begin(), {"--out", "x.csv"});
    const Outcome o = export_(std::move(args));
    EXPECT_EQ(o.code, elctl::kUsage);
    EXPECT_TRUE(contains(o.err, "usage: elctl export")) << o.err;
    return first_line(o.err);
  };
  EXPECT_EQ(says({"--from", "yesterday"}), head + "--from takes a UTC date YYYY-MM-DD; got 'yesterday'");
  EXPECT_EQ(says({"--to", "2026-02-30"}), head + "--to takes a UTC date YYYY-MM-DD; got '2026-02-30'");
  EXPECT_EQ(says({"--from", "2026-10-03", "--to", "2026-10-02"}), head + "--to is before --from");
  EXPECT_EQ(says({"--limit", "0"}), head + "--limit takes a whole positive number");
  EXPECT_EQ(says({"--constants", "mine"}), head + "--constants is default, legacy or legacy_preferences; got 'mine'");
  EXPECT_EQ(says({"--plateau", "york"}), head + "--plateau is fleck or mahon");
  EXPECT_EQ(says({"--plateau-steps", "1"}), head + "--plateau-steps takes a whole number of 2 or more");
  EXPECT_EQ(says({"--plateau-gas", "101"}), head + "--plateau-gas takes a percent, 0 to 100");
  EXPECT_EQ(says({"--uuid"}), head + "--uuid needs a value");
  EXPECT_EQ(says({"--frob"}), head + "--frob needs a value");
  EXPECT_EQ(says({"--frob", "1"}), head + "unknown flag '--frob'");
  EXPECT_EQ(says({"stray"}), head + "unexpected argument 'stray'");
  // Left to right, across the flags that moved and those that stayed.
  EXPECT_EQ(says({"--sigma", "3", "--from", "yesterday"}), head + "--sigma is 1 or 2; got '3'");
  EXPECT_EQ(says({"--from", "yesterday", "--sigma", "3"}), head + "--from takes a UTC date YYYY-MM-DD; got 'yesterday'");
  EXPECT_EQ(says({"--frob", "1", "--limit", "0"}), head + "unknown flag '--frob'");
  // A value is a value, whatever it looks like.
  EXPECT_EQ(says({"--sample", "--include-invalid", "--plateau", "york"}), head + "--plateau is fleck or mahon");

  const Outcome db_first = run_raw({"export", "--from", "2026-10-03", "--to", "2026-10-02"});
  EXPECT_EQ(first_line(db_first.err), head + "--db <url> is required");

  // A value that looks like a flag is taken as the value: no sample of that
  // name, not a usage error. And the flags still narrow.
  const Outcome odd = export_({"--out", path("odd.csv").string(), "--sample", "--decay-error"});
  EXPECT_EQ(odd.code, elctl::kFailed) << odd.err;
  EXPECT_EQ(odd.err, "elctl export: no analysis matched\n");
  const Outcome narrowed = export_({"--out", path("narrowed.csv").string(), "--sample", "FC-2", "--identifier", "77000",
                                    "--project", "Fish Canyon", "--irradiation", "NM-300", "--limit", "1", "--decay-error",
                                    "--constants", "legacy", "--plateau", "mahon", "--plateau-steps", "2", "--plateau-gas",
                                    "40"});
  EXPECT_EQ(narrowed.code, elctl::kOk) << narrowed.err;
  EXPECT_TRUE(contains(narrowed.out, "1 analyses, 1 group")) << narrowed.out;
}

}  // namespace

#endif
