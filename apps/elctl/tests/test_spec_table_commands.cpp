// elctl spec table: the read commands (list, show, versions, diff, check),
// on a scratch copy of the example lab and its field tables.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "elctl_fixture.hpp"

namespace elctl::testing {
namespace {

namespace fs = std::filesystem;

// The shipped argon table with H1 at Ar40 moved, Ar36 dropped and Ar38 added.
constexpr const char* kSecondArgon = R"(fit = "quadratic"
axis = "dac"

[[points]]
isotope = "Ar40"
mass = 39.9623831237
H2 = 5.123
H1 = 5.011
AX = 4.874
L1 = 4.750
L2 = 4.628
CDD = 4.505

[[points]]
isotope = "Ar39"
mass = 38.964313
H2 = 4.995
H1 = 4.874
AX = 4.750
L1 = 4.628
L2 = 4.505
CDD = 4.383

[[points]]
isotope = "Ar38"
mass = 37.9627322
H2 = 4.870
H1 = 4.750
AX = 4.628
L1 = 4.505
L2 = 4.383
CDD = 4.264
)";

// Ar40 twice: an error of check_field_table.
constexpr const char* kDuplicateArgon = R"(fit = "linear"
axis = "dac"

[[points]]
isotope = "Ar40"
mass = 39.9623831237
H2 = 5.123
H1 = 5.001
AX = 4.874
L1 = 4.750
L2 = 4.628
CDD = 4.505

[[points]]
isotope = "Ar40"
mass = 38.964313
H2 = 4.995
H1 = 4.874
AX = 4.750
L1 = 4.628
L2 = 4.505
CDD = 4.383
)";

class SpecTableCmd : public ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_ / "lab",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::remove_all(dir_ / "lab" / "data");
  }
  fs::path lab(const std::string& f) const { return dir_ / "lab" / f; }
  fs::path table_dir(const std::string& name) const { return lab("tables") / name; }

  Outcome table(std::vector<std::string> args) const {
    std::vector<std::string> all{"-c", lab("extraction_line.toml").string(), "spec", "table"};
    all.insert(all.end(), args.begin(), args.end());
    all.emplace_back("--spectrometer");
    all.emplace_back(lab("spectrometer.qtegra.toml").string());
    return run_raw(all);
  }

  // Writes a hand-made version of `name` and, with `make_current`, points
  // `current` at it.
  void add_version(const std::string& name, const std::string& version, const std::string& text,
                   bool make_current) const {
    std::ofstream(table_dir(name) / (version + ".toml"), std::ios::binary) << text;
    if (make_current) std::ofstream(table_dir(name) / "current", std::ios::binary | std::ios::trunc) << version << ".toml\n";
  }
};

TEST_F(SpecTableCmd, ListMarksActiveAndHv) {
  const auto o = table({"list"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "argon  2026-09-30T000000  (active)\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "argon_hv  2026-09-30T000000  (HV)\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "argon_legacy  2026-09-30T000000\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "ngx_argon  2026-10-03T000000\n")) << o.out;
}

TEST_F(SpecTableCmd, ShowDefaultsToActiveCurrent) {
  add_version("argon", "20261001T000000Z", kSecondArgon, false);
  const auto o = table({"show"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "argon 2026-09-30T000000")) << o.out;
  // Header, then rows by mass: Ar36 first.
  const auto header = o.out.find("Isotope");
  ASSERT_NE(header, std::string::npos) << o.out;
  EXPECT_TRUE(contains(o.out.substr(header, o.out.find('\n', header) - header), "Mass")) << o.out;
  const auto ar36 = o.out.find("Ar36");
  const auto ar39 = o.out.find("Ar39");
  const auto ar40 = o.out.find("Ar40");
  ASSERT_NE(ar36, std::string::npos) << o.out;
  EXPECT_LT(header, ar36);
  EXPECT_LT(ar36, ar39);
  EXPECT_LT(ar39, ar40);
  EXPECT_TRUE(contains(o.out, "5.00100")) << o.out;
  EXPECT_FALSE(contains(o.out, "5.01100")) << o.out;
  EXPECT_TRUE(contains(o.out, "fit: quadratic")) << o.out;
}

TEST_F(SpecTableCmd, ShowAVersion) {
  add_version("argon", "20261001T000000Z", kSecondArgon, false);
  const auto o = table({"show", "argon", "--version", "20261001T000000Z"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "5.01100")) << o.out;
  EXPECT_TRUE(contains(o.out, "Ar38")) << o.out;
  EXPECT_FALSE(contains(o.out, "Ar36")) << o.out;

  // A detector with no value in a row is shown as "-".
  add_version("argon", "20261002T000000Z",
              "fit = \"discrete\"\naxis = \"dac\"\n[[points]]\nisotope = \"Ar40\"\nmass = 39.9623831237\nH1 = 5.0\n"
              "[[points]]\nisotope = \"Ar39\"\nmass = 38.964313\nH2 = 4.9\n",
              false);
  const auto sparse = table({"show", "argon", "--version", "20261002T000000Z"});
  ASSERT_EQ(sparse.code, 0) << sparse.err;
  EXPECT_TRUE(contains(sparse.out, " -")) << sparse.out;
}

TEST_F(SpecTableCmd, VersionsNewestFirst) {
  add_version("argon", "20261001T000000Z", kSecondArgon, false);
  add_version("argon", "20261002T000000Z", kSecondArgon, true);
  const auto o = table({"versions", "argon"});
  ASSERT_EQ(o.code, 0) << o.err;
  const auto newest = o.out.find("20261002T000000Z");
  const auto middle = o.out.find("20261001T000000Z");
  const auto oldest = o.out.find("2026-09-30T000000");
  ASSERT_NE(newest, std::string::npos) << o.out;
  ASSERT_NE(middle, std::string::npos) << o.out;
  ASSERT_NE(oldest, std::string::npos) << o.out;
  EXPECT_LT(newest, middle);
  EXPECT_LT(middle, oldest);
  EXPECT_TRUE(contains(o.out, "20261002T000000Z  (current)")) << o.out;
  EXPECT_FALSE(contains(o.out, "20261001T000000Z  (current)")) << o.out;
}

TEST_F(SpecTableCmd, DiffByIsotope) {
  add_version("argon", "20261001T000000Z", kSecondArgon, true);
  // v2 defaults to current.
  const auto o = table({"diff", "argon", "2026-09-30T000000"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "+ Ar38\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "- Ar36\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "Ar40 H1 5.00100 -> 5.01100 (\xCE\x94 +0.01000)")) << o.out;
  // Ar39 is the same on both sides.
  EXPECT_FALSE(contains(o.out, "Ar39")) << o.out;

  const auto same = table({"diff", "argon", "20261001T000000Z", "20261001T000000Z"});
  ASSERT_EQ(same.code, 0) << same.err;
  EXPECT_TRUE(contains(same.out, "no differences")) << same.out;
}

TEST_F(SpecTableCmd, CheckCleanTableExitsZero) {
  const auto o = table({"check"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_FALSE(contains(o.out, "error:")) << o.out;
  EXPECT_TRUE(contains(o.out, "argon 2026-09-30T000000")) << o.out;
}

TEST_F(SpecTableCmd, CheckErrorExitsOne) {
  add_version("argon", "20261001T000000Z", kDuplicateArgon, true);
  const auto o = table({"check", "argon"});
  EXPECT_EQ(o.code, kFailed) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "error: row 2: ")) << o.out;
  EXPECT_TRUE(contains(o.out, "Ar40")) << o.out;
}

TEST_F(SpecTableCmd, UnknownTableFails) {
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"show", "nope"}, {"versions", "nope"}, {"check", "nope"}, {"diff", "nope", "x"}}) {
    const auto o = table(args);
    EXPECT_EQ(o.code, kFailed) << args.front() << ": " << o.out << o.err;
    EXPECT_TRUE(contains(o.err, "no field table 'nope'")) << args.front() << ": " << o.err;
  }
  const auto version = table({"show", "argon", "--version", "19990101T000000Z"});
  EXPECT_EQ(version.code, kFailed) << version.out << version.err;
}

TEST_F(SpecTableCmd, BadArgumentsAreUsage) {
  EXPECT_EQ(table({}).code, kUsage);
  EXPECT_EQ(table({"frobnicate"}).code, kUsage);
  EXPECT_EQ(table({"versions"}).code, kUsage);
  EXPECT_EQ(table({"diff", "argon"}).code, kUsage);
  EXPECT_EQ(table({"show", "--version"}).code, kUsage);
  // No spectrometer file given and no install: nothing to read.
  const auto none = run_raw({"-c", lab("extraction_line.toml").string(), "spec", "table", "list"});
  EXPECT_EQ(none.code, kUsage) << none.err;
}

}  // namespace
}  // namespace elctl::testing
