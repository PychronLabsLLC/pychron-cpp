// elctl spec table: the read commands (list, show, versions, diff, check)
// and the write commands (set, restore, import), on a scratch copy of the example lab and its field tables.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
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

  std::string read_file(const fs::path& p) const {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  }
  std::string current_of(const std::string& name) const { return read_file(table_dir(name) / "current"); }
  std::size_t version_count(const std::string& name) const {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(table_dir(name)))
      if (e.path().extension() == ".toml") ++n;
    return n;
  }
  // Takes detector H2 out of the scratch spectrometer config, as at a lab
  // whose mftable.csv still has a column the config no longer names.
  void drop_h2_from_config() const {
    const auto file = lab("spectrometer.qtegra.toml");
    auto text = read_file(file);
    const auto start = text.find("[[detectors]]\nname = \"H2\"");
    ASSERT_NE(start, std::string::npos);
    const auto next = text.find("[[detectors]]", start + 1);
    ASSERT_NE(next, std::string::npos);
    text.erase(start, next - start);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
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

TEST_F(SpecTableCmd, SetWritesANewVersionAndPropagates) {
  const auto before = version_count("argon");
  const auto o = table({"set", "argon", "H1", "Ar40", "5.011", "--propagate"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_EQ(version_count("argon"), before + 1);
  EXPECT_NE(current_of("argon"), "2026-09-30T000000.toml\n");
  // H1 moved by +0.010; with --propagate every other detector at Ar40 did too.
  const auto shown = table({"show", "argon"});
  ASSERT_EQ(shown.code, 0) << shown.err;
  EXPECT_TRUE(contains(shown.out, "5.01100")) << shown.out;
  EXPECT_TRUE(contains(shown.out, "5.13300")) << shown.out;  // H2 5.123 + 0.010

  // Without it (the config default here) only H1 moves.
  const auto lone = table({"set", "argon", "H1", "Ar40", "5.021"});
  ASSERT_EQ(lone.code, 0) << lone.out << lone.err;
  const auto again = table({"show", "argon"});
  EXPECT_TRUE(contains(again.out, "5.02100")) << again.out;
  EXPECT_TRUE(contains(again.out, "5.13300")) << again.out;
}

TEST_F(SpecTableCmd, SetRefusedOnDiagnosticErrorExitsOne) {
  const auto before = version_count("argon");
  const auto current = current_of("argon");
  const auto o = table({"set", "argon", "H1", "Ar40", "nan"});
  EXPECT_EQ(o.code, kFailed) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "error: row ")) << o.out;
  EXPECT_TRUE(contains(o.out, "is not finite")) << o.out;
  EXPECT_FALSE(contains(o.out, "wrote ")) << o.out;
  EXPECT_EQ(version_count("argon"), before);
  EXPECT_EQ(current_of("argon"), current);

  // Neither a detector nor an isotope the table lacks is written.
  EXPECT_EQ(table({"set", "argon", "H9", "Ar40", "5.0"}).code, kFailed);
  EXPECT_EQ(table({"set", "argon", "H1", "Kr84", "5.0"}).code, kFailed);
  EXPECT_EQ(table({"set", "argon", "H1", "Ar40", "five"}).code, kUsage);
  EXPECT_EQ(version_count("argon"), before);
}

TEST_F(SpecTableCmd, ImportTheMelbourneFixture) {
  drop_h2_from_config();
  const auto o = table({"import", "argon", std::string(PYCHRON_SYSTEMS_TEST_DATA_DIR) + "/mftable_melbourne.csv"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "warning: H2: ")) << o.out;
  EXPECT_TRUE(contains(o.out, "wrote argon ")) << o.out;
  const auto shown = table({"show", "argon"});
  ASSERT_EQ(shown.code, 0) << shown.err;
  EXPECT_TRUE(contains(shown.out, "6.24000")) << shown.out;  // H1 at Ar40
  EXPECT_TRUE(contains(shown.out, "(axis dac)")) << shown.out;
  EXPECT_TRUE(contains(shown.out, "fit: quadratic")) << shown.out;
}

TEST_F(SpecTableCmd, ImportRefusedOnError) {
  const auto csv = dir_ / "dup.csv";
  std::ofstream(csv, std::ios::binary) << "iso,H1,AX\nAr40,5.0,4.9\nAr40,4.8,4.7\n";
  const auto before = version_count("argon");
  const auto current = current_of("argon");
  const auto o = table({"import", "argon", csv.string()});
  EXPECT_EQ(o.code, kFailed) << o.out << o.err;
  EXPECT_TRUE(contains(o.err, "duplicate isotope Ar40")) << o.err;
  EXPECT_FALSE(contains(o.out, "wrote ")) << o.out;
  EXPECT_EQ(version_count("argon"), before);
  EXPECT_EQ(current_of("argon"), current);

  const auto missing = table({"import", "argon", (dir_ / "nope.csv").string()});
  EXPECT_EQ(missing.code, kFailed) << missing.out << missing.err;
}

TEST_F(SpecTableCmd, RestoreMovesPointer) {
  add_version("argon", "20261001T000000Z", kSecondArgon, true);
  const auto o = table({"restore", "argon", "2026-09-30T000000"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(current_of("argon"), "2026-09-30T000000")) << current_of("argon");
  EXPECT_EQ(version_count("argon"), 2U);

  EXPECT_EQ(table({"restore", "argon", "19990101T000000Z"}).code, kFailed);
}

TEST_F(SpecTableCmd, RestoreToVersionWithErrorRefusedCurrentUnchanged) {
  add_version("argon", "20261001T000000Z", kDuplicateArgon, false);
  add_version("argon", "20261002T000000Z", "not = [toml", false);
  const auto current = current_of("argon");
  const auto o = table({"restore", "argon", "20261001T000000Z"});
  EXPECT_EQ(o.code, kFailed) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "error: row 2: ")) << o.out;
  EXPECT_EQ(current_of("argon"), current);

  const auto broken = table({"restore", "argon", "20261002T000000Z"});
  EXPECT_EQ(broken.code, kFailed) << broken.out << broken.err;
  EXPECT_EQ(current_of("argon"), current);
}

TEST_F(SpecTableCmd, OutputSaysWhenItTakesEffect) {
  const std::string effect = "; takes effect when a process next loads the table\n";
  const auto set = table({"set", "argon", "H1", "Ar40", "5.011"});
  ASSERT_EQ(set.code, 0) << set.out << set.err;
  EXPECT_TRUE(contains(set.out, "wrote argon ")) << set.out;
  EXPECT_TRUE(contains(set.out, effect)) << set.out;

  const auto restore = table({"restore", "argon", "2026-09-30T000000"});
  ASSERT_EQ(restore.code, 0) << restore.out << restore.err;
  EXPECT_TRUE(contains(restore.out, "wrote argon 2026-09-30T000000" + effect)) << restore.out;
}

TEST_F(SpecTableCmd, BadArgumentsAreUsage) {
  EXPECT_EQ(table({}).code, kUsage);
  EXPECT_EQ(table({"frobnicate"}).code, kUsage);
  EXPECT_EQ(table({"versions"}).code, kUsage);
  EXPECT_EQ(table({"diff", "argon"}).code, kUsage);
  EXPECT_EQ(table({"show", "--version"}).code, kUsage);
  EXPECT_EQ(table({"set", "argon", "H1", "Ar40"}).code, kUsage);
  EXPECT_EQ(table({"set", "argon", "H1", "Ar40", "5.0", "--propagate", "--no-propagate"}).code, kUsage);
  EXPECT_EQ(table({"show", "--propagate"}).code, kUsage);
  EXPECT_EQ(table({"restore", "argon"}).code, kUsage);
  EXPECT_EQ(table({"import", "argon"}).code, kUsage);
  // No spectrometer file given and no install: nothing to read.
  const auto none = run_raw({"-c", lab("extraction_line.toml").string(), "spec", "table", "list"});
  EXPECT_EQ(none.code, kUsage) << none.err;
}

}  // namespace
}  // namespace elctl::testing
