// elctl earthbank: the AusGeochem workbooks from a file-backed SQLite store,
// driven through elctl::run. Nothing here, and nothing the command links,
// talks to the service.

#include <gtest/gtest.h>

#include "earthbank.hpp"
#include "elctl_fixture.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_HAS_STORE

TEST(ElctlEarthbank, StubWithoutPersistence) {
  const Outcome o = run_raw({"earthbank", "export-xlsx", "--db", "sqlite::memory:", "--out", "x"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
  EXPECT_EQ(o.out, "");
}

// The commands that talk to the service say what is missing: the network
// first, when this build has none either.
TEST(ElctlEarthbank, TheNetworkCommandsWithoutPersistence) {
  for (const char* sub : {"login", "account", "packages", "preflight", "upload"}) {
    const Outcome o = run_raw({"earthbank", sub});
    EXPECT_EQ(o.code, elctl::kUsage) << sub;
    EXPECT_EQ(o.out, "") << sub;
#ifdef PYCHRON_ELCTL_HAS_NETWORK
    EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
#else
    EXPECT_TRUE(contains(o.err, "this build has no network support (Qt6 Network was not found)")) << o.err;
#endif
  }
}

#else

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_earthbank.hpp"
#include "ooxml.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "session_support.hpp"
#include "store_fixture.hpp"

namespace {

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
namespace ooxml = pychron::earthbank::ooxml;

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::vector<fs::path> files_in(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<fs::path> named(const std::vector<fs::path>& files, const std::string& start) {
  std::vector<fs::path> found;
  for (const auto& f : files)
    if (f.filename().string().starts_with(start)) found.push_back(f);
  return found;
}

// The data rows of a sheet of a written workbook: row 4 names the columns,
// the rows from 5 hold the values, down to the first that holds none.
using Row = std::map<std::string, std::string>;
std::vector<Row> rows_of(const fs::path& file, const std::string& sheet) {
  std::vector<Row> rows;
  auto book = ooxml::Workbook::open(file);
  EXPECT_TRUE(book) << file << ": " << (book ? "" : book.error().what);
  if (!book) return rows;
  const auto keys = book->row_values(sheet, 4);
  EXPECT_TRUE(keys) << sheet << ": " << (keys ? "" : keys.error().what);
  if (!keys) return rows;
  for (int r = 5;; ++r) {
    const auto values = book->row_values(sheet, r);
    if (!values || values->empty()) break;
    Row row;
    for (std::size_t c = 0; c < values->size() && c < keys->size(); ++c)
      if (!(*values)[c].empty()) row[(*keys)[c]] = (*values)[c];
    if (row.empty()) break;
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string at(const Row& row, const std::string& key) {
  const auto it = row.find(key);
  return it == row.end() ? std::string() : it->second;
}

// The seeded store (FC-2: two heating steps of one aliquot, and an air) with
// a second sample, S-FUS: two fusions.
class ElctlEarthbank : public elctl::testing::StoreTest {
 protected:
  void SetUp() override {
    StoreTest::SetUp();
    // The templates the source tree holds, whatever the environment says;
    // what it said is put back afterwards.
    if (const char* was = std::getenv(kTemplatesVariable)) templates_were_ = was;
    set_templates_variable(std::nullopt);
    fused_ = *store_->add_sample(acq_, {.name = "S-FUS", .project = project_, .material = material_});
    unknown_at(4, fused_, "77001");
    ingest("77001", 1, -1, "unknown", "2026-10-03T09:00:00Z", 1200.0);
    ingest("77001", 2, -1, "unknown", "2026-10-03T09:30:00Z", 1210.0);
    out_ = path("out");
  }

  void TearDown() override {
    set_templates_variable(templates_were_);
    StoreTest::TearDown();
  }

  static void set_templates_variable(const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(kTemplatesVariable, value ? value->c_str() : "");
#else
    if (value) {
      setenv(kTemplatesVariable, value->c_str(), 1);
    } else {
      unsetenv(kTemplatesVariable);
    }
#endif
  }

  Outcome earthbank(std::vector<std::string> args) const {
    args.insert(args.begin(), {"earthbank", "export-xlsx", "--db", db_});
    return run_raw(std::move(args));
  }

  Outcome to_out(std::vector<std::string> args = {}) const {
    args.insert(args.begin(), {"--out", out_.string()});
    return earthbank(std::move(args));
  }

  std::string overrides(const std::string& text) const {
    write("overrides.toml", text);
    return path("overrides.toml").string();
  }

  static constexpr const char* kTemplatesVariable = "PYCHRON_EARTHBANK_TEMPLATES";
  std::optional<std::string> templates_were_;
  ps::Uuid fused_;
  fs::path out_;
};

TEST_F(ElctlEarthbank, HelpNamesTheCommandAndTheOverrides) {
  for (const auto& args : std::vector<std::vector<std::string>>{{"earthbank", "help"}, {"earthbank", "--help"},
                                                                {"earthbank", "export-xlsx", "--help"}}) {
    const Outcome help = run_raw(args);
    EXPECT_EQ(help.code, elctl::kOk);
    EXPECT_TRUE(contains(help.out, "export-xlsx")) << help.out;
    EXPECT_TRUE(contains(help.out, "--overrides")) << help.out;
    EXPECT_TRUE(contains(help.out, "by hand")) << help.out;
    EXPECT_EQ(help.err, "");
  }
  EXPECT_TRUE(contains(run_raw({"help"}).out, "earthbank export-xlsx"));
  // Help is of a subcommand there is.
  const Outcome frob = run_raw({"earthbank", "frob", "--help"});
  EXPECT_EQ(frob.code, elctl::kUsage);
  EXPECT_EQ(frob.out, "");
  EXPECT_TRUE(contains(frob.err, "elctl earthbank: unknown subcommand 'frob'\n")) << frob.err;
  // The ages are export's when export is asked the same thing, and the
  // environment's templates come before the installed ones.
  const std::string text = run_raw({"earthbank", "help"}).out;
  EXPECT_TRUE(contains(text, "--group-by sample --sigma 1")) << text;
  EXPECT_LT(text.find("$PYCHRON_EARTHBANK_TEMPLATES"), text.find("installed ones")) << text;
  EXPECT_TRUE(contains(text, "UTC")) << text;
}

TEST_F(ElctlEarthbank, ExportXlsxWritesBothWorkbooks) {
  const Outcome o = to_out();
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_EQ(o.err, "");

  const auto files = files_in(out_);
  ASSERT_EQ(files.size(), 2u) << o.out;
  const auto arar = named(files, "ArArDataPoint_");
  const auto samples = named(files, "Samples_");
  ASSERT_EQ(arar.size(), 1u);
  ASSERT_EQ(samples.size(), 1u);
  // Each file is said, and nothing else is said to be written.
  EXPECT_TRUE(contains(o.out, "wrote " + arar[0].string() + "\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "wrote " + samples[0].string() + "\n")) << o.out;

  // One measurement row per analysis, under its data point; the air is not
  // an unknown and is not there.
  const auto measurements = rows_of(arar[0], "ArArMeasurement");
  ASSERT_EQ(measurements.size(), 4u);
  std::map<std::string, int> per_datapoint;
  for (const auto& row : measurements) ++per_datapoint[at(row, "datapointName")];
  EXPECT_EQ(per_datapoint, (std::map<std::string, int>{{"FC-2 sanidine", 2}, {"S-FUS sanidine", 2}}));
  EXPECT_EQ(rows_of(arar[0], "ArArDataPoints").size(), 2u);
  EXPECT_EQ(rows_of(arar[0], "ArArAgeSummary").size(), 2u);
  const auto sample_rows = rows_of(samples[0], "Samples");
  ASSERT_EQ(sample_rows.size(), 2u);
  EXPECT_EQ(at(sample_rows[0], "sampleName"), "FC-2");
  EXPECT_EQ(at(sample_rows[1], "sampleName"), "S-FUS");

  // One line per sample: its analyses and the age reported, then its notes,
  // the choice of age among them.
  EXPECT_TRUE(contains(o.out, "FC-2: 2 analyses, Total Gas Age ")) << o.out;
  EXPECT_TRUE(contains(o.out, "S-FUS: 2 analyses, Weighted Mean Age ")) << o.out;
  EXPECT_TRUE(contains(o.out, " Ma (1 sigma)\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "\n  preferred age: Total Gas Age")) << o.out;
  EXPECT_TRUE(contains(o.out, "\n  preferred age: Weighted Mean Age")) << o.out;
  // The sentence that goes into the workbook as the age's description is
  // said to be that, once per sample.
  EXPECT_TRUE(contains(o.out, "\n  description: Total Gas Age of 2 of 2 analyses; ")) << o.out;
  EXPECT_TRUE(contains(o.out, "\n  description: Weighted Mean Age of 2 of 2 analyses; ")) << o.out;
  EXPECT_FALSE(contains(o.out, "\n  Total Gas Age of 2 of 2 analyses")) << o.out;
  const auto described = rows_of(arar[0], "ArArAgeSummary");
  ASSERT_EQ(described.size(), 2u);
  for (const auto& row : described)
    EXPECT_TRUE(contains(o.out, "\n  description: " + at(row, "description") + "\n")) << at(row, "description");
  // A submission's notes come under its sample, before the files; the
  // export's notes once, after them.
  const auto wrote = o.out.find("wrote ");
  const auto setup_note = o.out.find("`setup` setting");
  const auto export_note = o.out.find("\nnote: ");
  ASSERT_NE(setup_note, std::string::npos) << o.out;
  ASSERT_NE(export_note, std::string::npos) << o.out;
  EXPECT_LT(setup_note, wrote);
  EXPECT_GT(export_note, wrote);
  EXPECT_TRUE(contains(o.out.substr(export_note), "arArSetUp")) << o.out;

  // A second export overwrites nothing.
  const Outcome again = to_out();
  EXPECT_EQ(again.code, elctl::kOk) << again.err;
  EXPECT_EQ(files_in(out_).size(), 4u);
}

TEST_F(ElctlEarthbank, PerSampleAndNoSamples) {
  const Outcome o = to_out({"--per-sample", "--no-samples", "--templates", PYCHRON_EARTHBANK_TEMPLATE_DIR});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  const auto files = files_in(out_);
  ASSERT_EQ(files.size(), 2u) << o.out;
  EXPECT_EQ(files[0].filename(), "ArArDataPoint_FC-2.xlsx");
  EXPECT_EQ(files[1].filename(), "ArArDataPoint_S-FUS.xlsx");
  EXPECT_EQ(rows_of(files[0], "ArArMeasurement").size(), 2u);
  EXPECT_EQ(rows_of(files[1], "ArArMeasurement").size(), 2u);
  EXPECT_FALSE(contains(o.out, "/Samples_")) << o.out;

  // The selection narrows as it does for `export`.
  const fs::path one = path("one");
  const Outcome narrowed = earthbank({"--out", one.string(), "--sample", "S-FUS", "--no-samples"});
  EXPECT_EQ(narrowed.code, elctl::kOk) << narrowed.err;
  EXPECT_FALSE(contains(narrowed.out, "FC-2: ")) << narrowed.out;
  ASSERT_EQ(files_in(one).size(), 1u);
  EXPECT_EQ(rows_of(files_in(one)[0], "ArArMeasurement").size(), 2u);

  // Templates that are not where --templates says: nothing written.
  const fs::path nowhere = path("nowhere");
  const Outcome missing = earthbank({"--out", nowhere.string(), "--templates", path("no-templates").string()});
  EXPECT_EQ(missing.code, elctl::kUsage);
  EXPECT_TRUE(contains(missing.err, "elctl earthbank: ")) << missing.err;
  EXPECT_TRUE(contains(missing.err, "no-templates")) << missing.err;
  EXPECT_FALSE(fs::exists(nowhere));
}

TEST_F(ElctlEarthbank, OverridesFileIsApplied) {
  const std::string file = overrides(R"toml(
setup = "Example Lab ARGUS VI"
decay_constant = "Min et al. 2000"
air_ratio = "Lee et al. 2006"
age_includes_j_error = false

[samples."S-FUS"]
interpretation = "Eruption age"
preferred_age = "integrated"
flux_monitor = "Fish Canyon Tuff sanidine Renne et al. 2011"
datapoint_name = "S-FUS fusions"
irradiation = "NM-300 (example)"
)toml");
  const Outcome o = to_out({"--overrides", file});
  ASSERT_EQ(o.code, elctl::kOk) << o.err;
  const auto arar = named(files_in(out_), "ArArDataPoint_");
  ASSERT_EQ(arar.size(), 1u);

  const auto points = rows_of(arar[0], "ArArDataPoints");
  ASSERT_EQ(points.size(), 2u);
  EXPECT_EQ(at(points[0], "datapointName"), "FC-2 sanidine");
  EXPECT_EQ(at(points[1], "datapointName"), "S-FUS fusions");
  // The set-up is the lab's, for every sample; the irradiation is said of one.
  EXPECT_EQ(at(points[0], "arArSetUp"), "Example Lab ARGUS VI");
  EXPECT_EQ(at(points[1], "arArSetUp"), "Example Lab ARGUS VI");
  EXPECT_EQ(at(points[0], "irradiationBatchID"), "NM-300");
  EXPECT_EQ(at(points[1], "irradiationBatchID"), "NM-300 (example)");

  const auto calcs = rows_of(arar[0], "ArArAgeCalc");
  ASSERT_EQ(calcs.size(), 2u);
  for (const auto& calc : calcs) {
    EXPECT_EQ(at(calc, "decayConstant"), "Min et al. 2000");
    EXPECT_EQ(at(calc, "airRatio"), "Lee et al. 2006");
  }
  // The monitor said for the one sample, by its name and its paper: written as
  // the entry of the template's list that it is (vocab.hpp match_flux_monitor).
  const std::string renne = "Fish Canyon Tuff Sanidine (FCTs) from Renne et al. 2011 (28.294 Ma)";
  EXPECT_NE(at(calcs[0], "fluxMonitor"), renne);
  EXPECT_EQ(at(calcs[1], "fluxMonitor"), renne);

  const auto ages = rows_of(arar[0], "ArArAgeSummary");
  ASSERT_EQ(ages.size(), 2u);
  EXPECT_EQ(at(ages[0], "ageCalcType"), "Total Gas Age");
  EXPECT_EQ(at(ages[0], "interpretation"), "Unknown");
  EXPECT_EQ(at(ages[1], "ageCalcType"), "Total Gas Age");
  EXPECT_EQ(at(ages[1], "interpretation"), "Eruption age");
  EXPECT_TRUE(contains(o.out, "S-FUS: 2 analyses, Total Gas Age ")) << o.out;
  EXPECT_TRUE(contains(o.out, "chosen in the options")) << o.out;
  // age_includes_j_error: the description of each age says which it is.
  for (const auto& age : ages)
    EXPECT_TRUE(contains(at(age, "description"), "analytical uncertainty only")) << at(age, "description");
  EXPECT_TRUE(contains(o.out, "without the J uncertainty")) << o.out;
  const Outcome with_j = earthbank({"--check"});
  EXPECT_TRUE(contains(with_j.out, "J uncertainty included")) << with_j.out;
  EXPECT_FALSE(contains(with_j.out, "without the J uncertainty")) << with_j.out;

  // Every kind of age has its word, and a sample left out of the file keeps
  // the rule. Two steps make no plateau and no isochron: the word is taken,
  // and the mapping says why that age is not to be had.
  const auto prefer = [&](const std::string& kind) {
    return earthbank({"--check", "--overrides", overrides("[samples.\"FC-2\"]\npreferred_age = \"" + kind + "\"\n")});
  };
  for (const std::string kind : {"weighted_mean", "integrated"}) {
    const Outcome chosen = prefer(kind);
    EXPECT_EQ(chosen.code, elctl::kOk) << kind << ": " << chosen.err;
    EXPECT_TRUE(contains(chosen.out, "chosen in the options")) << kind << ": " << chosen.out;
    EXPECT_TRUE(contains(chosen.out, "S-FUS: 2 analyses, Weighted Mean Age ")) << chosen.out;
  }
  EXPECT_TRUE(contains(prefer("weighted_mean").out, "FC-2: 2 analyses, Weighted Mean Age ")) << prefer("weighted_mean").out;
  EXPECT_TRUE(contains(prefer("plateau").err, "the Plateau Age asked for cannot be computed")) << prefer("plateau").err;
  EXPECT_TRUE(contains(prefer("isochron").err, "the Inverse Isochron Age asked for cannot be computed"))
      << prefer("isochron").err;
}

TEST_F(ElctlEarthbank, AnUnknownKeyOrSampleInOverridesIsRefused) {
  const auto refused = [&](const std::string& text) {
    const std::string file = overrides(text);
    const Outcome o = to_out({"--overrides", file});
    EXPECT_EQ(o.code, elctl::kUsage) << text;
    EXPECT_EQ(o.out, "") << text;
    EXPECT_TRUE(contains(o.err, "elctl earthbank: " + file + ": ")) << o.err;
    EXPECT_FALSE(fs::exists(out_)) << text;
    return o.err;
  };
  EXPECT_TRUE(contains(refused("decay_konstant = \"Min et al. 2000\"\n"), "unknown key 'decay_konstant'"));
  EXPECT_TRUE(contains(refused("[samples.\"FC-2\"]\ncolour = \"grey\"\n"), "unknown key 'samples.\"FC-2\".colour'"));
  EXPECT_TRUE(contains(refused("[sample.\"FC-2\"]\ninterpretation = \"Eruption age\"\n"), "unknown key 'sample'"));
  // A key of the wrong kind, and a word that is not a kind of age.
  EXPECT_TRUE(contains(refused("setup = 3\n"), "'setup' takes text"));
  EXPECT_TRUE(contains(refused("age_includes_j_error = \"yes\"\n"), "'age_includes_j_error' takes true or false"));
  EXPECT_TRUE(contains(refused("samples = \"FC-2\"\n"), "'samples' takes a table"));
  EXPECT_TRUE(contains(refused("[samples]\n\"FC-2\" = \"plateau\"\n"), "'samples.\"FC-2\"' takes a table"));
  EXPECT_TRUE(contains(refused("[samples.\"FC-2\"]\npreferred_age = \"best\"\n"),
                       "'samples.\"FC-2\".preferred_age' is plateau, weighted_mean, isochron or integrated; got 'best'"));
  EXPECT_TRUE(contains(refused("setup = \n"), "overrides.toml"));  // not TOML at all
  // A sample the selection has not: named, with those it has.
  const std::string absent = refused("[samples.\"S-STEP\"]\ninterpretation = \"Eruption age\"\n");
  EXPECT_TRUE(contains(absent, "sample \"S-STEP\" is not among the selected analyses (FC-2, S-FUS)")) << absent;
  // The same file is right for a selection that has the sample, wrong for one that has not.
  const std::string for_fused = overrides("[samples.\"S-FUS\"]\ninterpretation = \"Eruption age\"\n");
  EXPECT_EQ(earthbank({"--check", "--overrides", for_fused}).code, elctl::kOk);
  const Outcome narrowed = earthbank({"--check", "--sample", "FC-2", "--overrides", for_fused});
  EXPECT_EQ(narrowed.code, elctl::kUsage);
  EXPECT_TRUE(contains(narrowed.err, "sample \"S-FUS\" is not among the selected analyses (FC-2)")) << narrowed.err;

  // A file that is not there, and a path that is no file: neither is an
  // empty set of overrides.
  const Outcome no_file = to_out({"--overrides", path("none.toml").string()});
  EXPECT_EQ(no_file.code, elctl::kUsage);
  EXPECT_EQ(no_file.err, "elctl earthbank: " + path("none.toml").string() + ": no such file\n");
  fs::create_directories(path("overrides.d"));
  for (const bool check : {false, true}) {
    std::vector<std::string> args = {"--overrides", path("overrides.d").string()};
    if (check) args.emplace_back("--check");
    const Outcome directory = to_out(args);
    EXPECT_EQ(directory.code, elctl::kUsage);
    EXPECT_EQ(directory.err, "elctl earthbank: " + path("overrides.d").string() + ": not a file\n");
    EXPECT_EQ(directory.out, "");
    EXPECT_FALSE(fs::exists(out_));
  }
  // An empty file is a file: no overrides.
  EXPECT_EQ(earthbank({"--check", "--overrides", overrides("")}).code, elctl::kOk);
}

// Twelve more samples: the message about a sample the selection has not
// names ten of those it has and counts the rest.
TEST_F(ElctlEarthbank, AnAbsentOverridesSampleListsAtMostTenOfThoseSelected) {
  for (int i = 0; i < 12; ++i) {
    const std::string n = std::to_string(10 + i);
    const auto sample = *store_->add_sample(acq_, {.name = "M-" + n, .project = project_, .material = material_});
    unknown_at(10 + i, sample, "780" + n);
    ingest("780" + n, 1, -1, "unknown", "2026-10-05T09:" + n + ":00Z", 1200.0);
  }
  const std::string file = overrides("[samples.\"S-STEP\"]\ninterpretation = \"Eruption age\"\n");
  const Outcome o = earthbank({"--check", "--overrides", file});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_EQ(o.err, "elctl earthbank: " + file +
                       ": sample \"S-STEP\" is not among the selected analyses (FC-2, M-10, M-11, M-12, M-13, M-14, "
                       "M-15, M-16, M-17, M-18 and 4 more)\n");
  // Ten are ten: nothing more to count.
  std::vector<std::string> args = {"--check", "--overrides", file};
  for (int i = 10; i < 20; ++i) args.insert(args.end(), {"--sample", "M-" + std::to_string(i)});
  const Outcome ten = earthbank(args);
  EXPECT_TRUE(contains(ten.err, "(M-10, M-11, M-12, M-13, M-14, M-15, M-16, M-17, M-18, M-19)\n")) << ten.err;
}

// The mapping and the export name a setting where one would resolve what
// they report (`... as the `setup` setting`). Each such name is a key of the
// overrides file, at the top or under a sample: a note never sends the user
// to a key the command refuses.
TEST_F(ElctlEarthbank, EverySettingANoteNamesIsAKeyOfTheOverridesFile) {
  std::set<std::string> settings;
  const std::regex named_setting("`([a-z_]+)` setting");
  for (const char* source : {"mapping.cpp", "xlsx.cpp"}) {
    const std::string text = slurp(fs::path(PYCHRON_EARTHBANK_SOURCE_DIR) / source);
    ASSERT_FALSE(text.empty()) << source;
    for (std::sregex_iterator it(text.begin(), text.end(), named_setting), end; it != end; ++it)
      settings.insert((*it)[1].str());
  }
  for (const char* known : {"setup", "decay_constant", "air_ratio", "irradiation"})
    EXPECT_TRUE(settings.contains(known)) << known << " is no longer named in a note";
  // The four known when this was written, and the one a refusal names;
  // more may come, and are held to the same.
  EXPECT_TRUE(settings.contains("preferred_age"));
  for (const auto& setting : settings) {
    // A key that is known is not said to be unknown, whatever is made of the
    // value `x`.
    const auto known = [&](const std::string& text) {
      return !contains(earthbank({"--check", "--overrides", overrides(text)}).err, "unknown key '");
    };
    const bool top = known(setting + " = \"x\"\n");
    const bool sample = known("[samples.\"FC-2\"]\n" + setting + " = \"x\"\n");
    EXPECT_TRUE(top || sample) << "`" << setting << "` is named in a note and is not a key of the overrides file";
    EXPECT_NE(top, sample) << setting << " is a key in both places";
  }
  // And the notes of this store say those names.
  const Outcome o = earthbank({"--check"});
  EXPECT_TRUE(contains(o.out, "`setup` setting")) << o.out;
}

TEST_F(ElctlEarthbank, NothingMatchedExitsOne) {
  for (const bool check : {false, true}) {
    std::vector<std::string> args = {"--out", out_.string(), "--sample", "no-such-sample"};
    if (check) args.emplace_back("--check");
    const Outcome none = earthbank(args);
    EXPECT_EQ(none.code, elctl::kFailed);
    EXPECT_EQ(none.err, "elctl earthbank: no analysis matched\n");
    EXPECT_EQ(none.out, "");
    EXPECT_FALSE(fs::exists(out_));
  }
  EXPECT_EQ(to_out({"--from", "2026-10-04"}).code, elctl::kFailed);
}

TEST_F(ElctlEarthbank, UsageErrorsExitTwo) {
  const auto usage = [&](std::vector<std::string> args, const std::string& says) {
    args.insert(args.begin(), "earthbank");
    const Outcome o = run_raw(args);
    EXPECT_EQ(o.code, elctl::kUsage) << says;
    EXPECT_EQ(o.out, "") << says;
    EXPECT_TRUE(contains(o.err, "elctl earthbank: " + says + "\n")) << o.err;
    EXPECT_TRUE(contains(o.err, "usage: elctl earthbank export-xlsx")) << o.err;
  };
  usage({}, "a subcommand is required");
  usage({"upload-everything"}, "unknown subcommand 'upload-everything'");
  usage({"export-xlsx", "--out", out_.string()}, "--db <url> is required");
  usage({"export-xlsx", "--db", db_}, "--out <dir> is required");
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--frob", "1"}, "unknown flag '--frob'");
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "stray"}, "unexpected argument 'stray'");
  usage({"export-xlsx", "--db", db_, "--out"}, "--out needs a value");
  // An empty word is not a file or a directory left to the default.
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--overrides", ""}, "--overrides needs a file");
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--templates", ""}, "--templates needs a directory");
  // `export`'s own flags that are not about selection or ages are not this command's.
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--sigma", "1"}, "unknown flag '--sigma'");
  // The shared flags say what they say for `export`.
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--from", "yesterday"},
        "--from takes a UTC date YYYY-MM-DD; got 'yesterday'");
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--plateau", "york"}, "--plateau is fleck or mahon");
  usage({"export-xlsx", "--db", db_, "--out", out_.string(), "--from", "2026-10-03", "--to", "2026-10-02"},
        "--to is before --from");
  EXPECT_FALSE(fs::exists(out_));

  // Fatal, not usage: said in one line, without the usage text.
  const Outcome missing = run_raw({"earthbank", "export-xlsx", "--db", "sqlite:" + path("nowhere.db").string(), "--out",
                                   out_.string()});
  EXPECT_EQ(missing.code, elctl::kUsage);
  EXPECT_TRUE(contains(missing.err, "elctl earthbank: no database at")) << missing.err;
  EXPECT_FALSE(contains(missing.err, "usage:")) << missing.err;
}

TEST_F(ElctlEarthbank, CheckSaysWhatTheExportWouldAndWritesNothing) {
  const Outcome check = earthbank({"--check"});  // --out is not needed
  EXPECT_EQ(check.code, elctl::kOk) << check.err;
  EXPECT_EQ(check.err, "");
  EXPECT_FALSE(contains(check.out, "wrote ")) << check.out;
  EXPECT_TRUE(contains(check.out, "nothing written (--check)\n")) << check.out;

  const Outcome with_out = to_out({"--check", "--per-sample"});
  EXPECT_EQ(with_out.code, elctl::kOk) << with_out.err;
  EXPECT_FALSE(fs::exists(out_));

  // But for the lines about files, the two say the same.
  const Outcome real = to_out();
  ASSERT_EQ(real.code, elctl::kOk) << real.err;
  const auto without_files = [](const std::string& text) {
    std::istringstream in(text);
    std::string kept, line;
    while (std::getline(in, line))
      if (!line.starts_with("wrote ") && !line.starts_with("nothing written")) kept += line + "\n";
    return kept;
  };
  EXPECT_EQ(without_files(check.out), without_files(real.out));
  EXPECT_TRUE(contains(check.out, "\nnote: ")) << check.out;
}

// FC-2 measured again from another position of the tray, whose flux was
// found against a monitor of another age: the two analyses of the sample do
// not agree about the monitor, and the mapping refuses the sample.
TEST_F(ElctlEarthbank, ADisagreeingSampleIsReportedAndTheOthersAreNotWritten) {
  unknown_at(5, sample_, "77002", 28.02);
  ingest("77002", 1, -1, "unknown", "2026-10-04T09:00:00Z", 1100.0);

  for (const auto& more : std::vector<std::vector<std::string>>{{}, {"--per-sample"}, {"--check"}}) {
    std::vector<std::string> args = {"--from", "2026-10-02"};
    args.insert(args.end(), more.begin(), more.end());
    const Outcome o = to_out(args);
    EXPECT_EQ(o.code, elctl::kUsage);
    EXPECT_TRUE(contains(o.err, "elctl earthbank: sample \"FC-2\": ")) << o.err;
    EXPECT_TRUE(contains(o.err, "flux monitor differs between analyses")) << o.err;
    EXPECT_TRUE(contains(o.err, "\"FC-2 28.201 Ma\" (77000-01A) and \"FC-2 28.02 Ma\" (77002-01)")) << o.err;
    EXPECT_FALSE(contains(o.err, "earthbank: earthbank:")) << o.err;
    // Nothing of S-FUS either, which is in order: no line, no file, no directory.
    EXPECT_EQ(o.out, "");
    EXPECT_FALSE(fs::exists(out_));
  }
  // The sample that is in order is written when it is selected alone.
  EXPECT_EQ(to_out({"--sample", "S-FUS"}).code, elctl::kOk);
}

// A sample with two step-heated aliquots: the mapping says which setting
// resolves it, and that setting, in the overrides file, does.
TEST_F(ElctlEarthbank, ARefusalNamesTheSampleAndWhatResolvesIt) {
  ingest("77000", 2, 0, "unknown", "2026-10-02T12:00:00Z", 1000.0);
  ingest("77000", 2, 1, "unknown", "2026-10-02T12:30:00Z", 1500.0);
  const Outcome o = to_out();
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl earthbank: sample \"FC-2\": ")) << o.err;
  EXPECT_TRUE(contains(o.err, "`preferred_age` setting to weighted_mean, integrated or isochron")) << o.err;
  EXPECT_FALSE(fs::exists(out_));

  const Outcome chosen = to_out({"--overrides", overrides("[samples.\"FC-2\"]\npreferred_age = \"weighted_mean\"\n")});
  EXPECT_EQ(chosen.code, elctl::kOk) << chosen.err;
  EXPECT_TRUE(contains(chosen.out, "FC-2: 4 analyses, Weighted Mean Age ")) << chosen.out;
}

// One selection, one set of flags, one age: what the workbook holds is what
// `elctl export` reports. FC-2 gets two more steps like its second, so the
// three make a plateau that holds 75% of the 39ArK.
TEST_F(ElctlEarthbank, TheAgesAreThoseExportReports) {
  ingest("77000", 1, 2, "unknown", "2026-10-02T11:10:00Z", 1500.0);
  ingest("77000", 1, 3, "unknown", "2026-10-02T11:40:00Z", 1500.0);

  struct Both {
    nlohmann::json summary, analyses;
    std::vector<Row> ages, measurements;
    std::string said;
  };
  int run = 0;
  const auto both = [&](std::vector<std::string> flags) {
    Both b;
    flags.insert(flags.begin(), {"--sample", "FC-2"});
    const fs::path json = path("report" + std::to_string(++run) + ".json");
    // A submission is of a sample and at one sigma: export is asked the same.
    std::vector<std::string> report_args = {"export", "--db", db_,      "--out", json.string(),
                                            "--group-by", "sample", "--sigma", "1"};
    report_args.insert(report_args.end(), flags.begin(), flags.end());
    const Outcome reported = run_raw(report_args);
    EXPECT_EQ(reported.code, elctl::kOk) << reported.err;
    const auto report = nlohmann::json::parse(slurp(json), nullptr, false);
    EXPECT_TRUE(report.is_object());
    if (!report.is_object()) return b;
    b.summary = report.at("summary").at(0);
    b.analyses = report.at("analyses");

    const fs::path dir = path("out" + std::to_string(run));
    std::vector<std::string> args = {"--out", dir.string(), "--no-samples"};
    args.insert(args.end(), flags.begin(), flags.end());
    const Outcome o = earthbank(args);
    EXPECT_EQ(o.code, elctl::kOk) << o.err;
    b.said = o.out;
    const auto files = files_in(dir);
    EXPECT_EQ(files.size(), 1u);
    if (files.empty()) return b;
    b.ages = rows_of(files[0], "ArArAgeSummary");
    b.measurements = rows_of(files[0], "ArArMeasurement");
    return b;
  };
  const auto same = [](const std::string& cell, const nlohmann::json& reported, const char* what) {
    ASSERT_TRUE(reported.is_number()) << what << ": the report has " << reported.dump();
    ASSERT_FALSE(cell.empty()) << what;
    EXPECT_GT(reported.get<double>(), 0.0) << what;
    EXPECT_NEAR(std::stod(cell), reported.get<double>(), 1e-9 * reported.get<double>()) << what;
  };

  // As both stand: the plateau, with the J uncertainty in its error, and the
  // steps' own ages, analytical.
  const Both plain = both({});
  ASSERT_EQ(plain.ages.size(), 1u);
  EXPECT_EQ(at(plain.ages[0], "ageCalcType"), "Plateau Age") << plain.said;
  same(at(plain.ages[0], "age"), plain.summary.at("plateau age (Ma)"), "plateau age");
  same(at(plain.ages[0], "ageUncertainty"), plain.summary.at("plateau age ±(1s) with J"), "plateau age, with J");
  EXPECT_EQ(std::stoi(at(plain.ages[0], "nAnalysisGrouped")), plain.summary.at("plateau n").get<int>());
  ASSERT_EQ(plain.measurements.size(), 4u);
  ASSERT_EQ(plain.analyses.size(), 4u);
  for (std::size_t i = 0; i < 4; ++i) {
    same(at(plain.measurements[i], "apparentAge"), plain.analyses[i].at("age (Ma)"), "age of a step");
    same(at(plain.measurements[i], "apparentAgeUncertainty"), plain.analyses[i].at("age (Ma) ±(1s) analytical"),
         "age of a step, analytical");
  }

  // The reduction flags reach both: other constants, other ages, still equal.
  const Both legacy = both({"--constants", "legacy", "--decay-error"});
  ASSERT_EQ(legacy.ages.size(), 1u);
  same(at(legacy.ages[0], "age"), legacy.summary.at("plateau age (Ma)"), "plateau age, legacy constants");
  same(at(legacy.ages[0], "ageUncertainty"), legacy.summary.at("plateau age ±(1s) with J"),
       "plateau age with J, legacy constants");
  EXPECT_NE(at(legacy.ages[0], "age"), at(plain.ages[0], "age"));

  // And the plateau flags: asked for more gas than the three steps hold,
  // neither has a plateau, and the workbook reports the report's integrated age.
  const Both strict = both({"--plateau-gas", "80"});
  ASSERT_EQ(strict.ages.size(), 1u);
  EXPECT_TRUE(strict.summary.at("plateau age (Ma)").is_null()) << strict.summary.dump();
  EXPECT_EQ(at(strict.ages[0], "ageCalcType"), "Total Gas Age") << strict.said;
  same(at(strict.ages[0], "age"), strict.summary.at("integrated age (Ma)"), "integrated age");
  same(at(strict.ages[0], "ageUncertainty"), strict.summary.at("integrated age ±(1s) with J"), "integrated age, with J");
  const Both four = both({"--plateau-steps", "4", "--plateau", "mahon"});
  EXPECT_TRUE(four.summary.at("plateau age (Ma)").is_null()) << four.summary.dump();
  ASSERT_EQ(four.ages.size(), 1u);
  EXPECT_EQ(at(four.ages[0], "ageCalcType"), "Total Gas Age") << four.said;
}

// ---------------------------------------------------------------- the account

// A password is never in what a command prints, and nothing here talks to the
// service but the fake: `earthbank_command` is given an EarthBank in memory
// and the memory store, and the profiles are a file of the test's own.
class ElctlEarthbankAccount : public ElctlEarthbank {
 protected:
  void SetUp() override {
    ElctlEarthbank::SetUp();
    if (const char* was = std::getenv(kPasswordVariable)) password_was_ = was;
    set_variable(std::nullopt);
    profiles_ = path("profiles.toml");
  }
  void TearDown() override {
    set_variable(password_was_);
    ElctlEarthbank::TearDown();
  }

  static void set_variable(const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(kPasswordVariable, value ? value->c_str() : "");
#else
    if (value) {
      setenv(kPasswordVariable, value->c_str(), 1);
    } else {
      unsetenv(kPasswordVariable);
    }
#endif
  }

  // `elctl earthbank <args>` against the fake and the memory store.
  Outcome eb(const std::vector<std::string>& args, const std::string& input = "") {
    std::istringstream in(input);
    std::ostringstream out, err;
    Outcome o;
    o.code = elctl::earthbank_command(args, elctl::Io{in, out, err}, elctl::EarthbankDeps{&spy_, secrets_.get()});
    o.out = out.str();
    o.err = err.str();
    outputs_.push_back(o.out);
    outputs_.push_back(o.err);
    return o;
  }

  // The same with the profiles file of this test.
  Outcome with_profiles(std::vector<std::string> args, const std::string& input = "") {
    args.insert(args.end(), {"--profiles", profiles_.string()});
    return eb(args, input);
  }

  Outcome log_in(const std::string& extra_profile = "mylab") {
    set_variable(bank_.password());
    const Outcome o = with_profiles({"login", "--profile", extra_profile, "--username", bank_.login(), "--base-url",
                                     bank_.base_url(), "--institution", "9301"});
    set_variable(std::nullopt);
    return o;
  }

  std::string kept_password(const std::string& name) {
    const auto profiles = pychron::earthbank::load_profiles(profiles_);
    if (!profiles || profiles->find(name) == nullptr) return "(no profile)";
    const auto kept = secrets_->get(pychron::earthbank::secret_account(*profiles->find(name)));
    return kept && *kept ? **kept : "(none)";
  }

  static constexpr const char* kPasswordVariable = "EARTHBANK_PASSWORD";
  pychron::earthbank::testing::FakeEarthBank bank_;
  pychron::earthbank::testing::Spy spy_{bank_};
  std::unique_ptr<pychron::earthbank::ISecretStore> secrets_ = pychron::earthbank::make_memory_secret_store();
  fs::path profiles_;
  std::optional<std::string> password_was_;
  std::vector<std::string> outputs_;  // everything every command printed
};

TEST_F(ElctlEarthbankAccount, LoginStoresThePasswordAndTheProfile) {
  const Outcome o = log_in();
  ASSERT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "signed in to https://earthbank.test as Robin Example (robin.example)")) << o.out;
  EXPECT_TRUE(contains(o.out, "profile 'mylab' saved")) << o.out;
  EXPECT_EQ(o.err, "");

  const auto profiles = pychron::earthbank::load_profiles(profiles_);
  ASSERT_TRUE(profiles) << profiles.error().what;
  EXPECT_EQ(profiles->active, "mylab");
  ASSERT_EQ(profiles->profiles.size(), 1u);
  const auto& p = profiles->profiles[0];
  EXPECT_EQ(p.name, "mylab");
  EXPECT_EQ(p.username, bank_.login());
  EXPECT_EQ(p.base_url, "https://earthbank.test");
  EXPECT_EQ(p.institution_id, std::optional<std::int64_t>(9301));
  // The password is kept by the store, under the profile's account, and in
  // no file.
  EXPECT_EQ(kept_password("mylab"), bank_.password());
  EXPECT_FALSE(contains(slurp(profiles_), bank_.password()));
  // The memory store lasts as long as the program, and the user is told.
  EXPECT_TRUE(contains(o.out, "note: this store lasts as long as the program")) << o.out;

  // A second login for the same profile keeps what the first said.
  set_variable(bank_.password());
  const Outcome again = with_profiles({"login", "--profile", "mylab"});
  set_variable(std::nullopt);
  ASSERT_EQ(again.code, elctl::kOk) << again.err;
  const auto reread = pychron::earthbank::load_profiles(profiles_);
  ASSERT_TRUE(reread);
  ASSERT_EQ(reread->profiles.size(), 1u);
  EXPECT_EQ(reread->profiles[0], p);
}

TEST_F(ElctlEarthbankAccount, LoginWithAWrongPasswordStoresNothing) {
  const std::string wrong = "a-wrong-password-4417";
  set_variable(wrong);
  const Outcome o = with_profiles({"login", "--profile", "mylab", "--username", bank_.login(), "--base-url",
                                   bank_.base_url()});
  EXPECT_EQ(o.code, elctl::kFailed) << o.err;
  EXPECT_EQ(o.out, "");
  EXPECT_FALSE(o.err.empty());
  EXPECT_FALSE(contains(o.err, wrong)) << o.err;
  EXPECT_FALSE(fs::exists(profiles_)) << "a profile was saved";
  const pychron::earthbank::Profile mine = [&] {
    pychron::earthbank::Profile p;
    p.name = "mylab";
    p.base_url = bank_.base_url();
    return p;
  }();
  const auto kept = secrets_->get(pychron::earthbank::secret_account(mine));
  ASSERT_TRUE(kept);
  EXPECT_FALSE(kept->has_value());
}

// Review focus 4: a script with no terminal and no variable is told, once,
// and never waits on its input: a password on the stream given is not read.
TEST_F(ElctlEarthbankAccount, LoginWithoutATerminalOrVariableExitsTwo) {
  const Outcome o = with_profiles({"login", "--profile", "mylab", "--username", bank_.login(), "--base-url",
                                   bank_.base_url()},
                                  bank_.password() + "\n");
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "no terminal")) << o.err;
  EXPECT_TRUE(contains(o.err, "$EARTHBANK_PASSWORD")) << o.err;
  EXPECT_EQ(o.out, "");
  EXPECT_TRUE(bank_.requests().empty()) << "something was sent";
  EXPECT_FALSE(fs::exists(profiles_));
  // An empty variable is not a password.
  set_variable("");
  EXPECT_EQ(with_profiles({"login", "--profile", "mylab", "--username", bank_.login(), "--base-url", bank_.base_url()})
                .code,
            elctl::kUsage);
  EXPECT_TRUE(bank_.requests().empty());
}

TEST_F(ElctlEarthbankAccount, LoginNeedsAUserNameAndAGoodAddress) {
  set_variable(bank_.password());
  const Outcome none = with_profiles({"login", "--profile", "mylab", "--base-url", bank_.base_url()});
  EXPECT_EQ(none.code, elctl::kUsage);
  EXPECT_TRUE(contains(none.err, "--username")) << none.err;
  const Outcome bad = with_profiles({"login", "--username", "u", "--base-url", "ftp://nowhere.test"});
  EXPECT_EQ(bad.code, elctl::kUsage);
  EXPECT_TRUE(contains(bad.err, "address")) << bad.err;
  const Outcome name = with_profiles({"login", "--profile", "my lab", "--username", "u"});
  EXPECT_EQ(name.code, elctl::kUsage);
  EXPECT_TRUE(bank_.requests().empty());
}

TEST_F(ElctlEarthbankAccount, AccountPrintsTheName) {
  ASSERT_EQ(log_in().code, elctl::kOk);
  const Outcome o = with_profiles({"account"});
  ASSERT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "account: Robin Example (robin.example), id 4107")) << o.out;
  EXPECT_TRUE(contains(o.out, "profile: mylab (https://earthbank.test)")) << o.out;
  // Without a login there is nothing to ask as.
  secrets_ = pychron::earthbank::make_memory_secret_store();
  const Outcome none = with_profiles({"account"});
  EXPECT_EQ(none.code, elctl::kUsage);
  EXPECT_TRUE(contains(none.err, "no password is kept")) << none.err;
  EXPECT_TRUE(contains(none.err, "elctl earthbank login")) << none.err;
  // The variable stands in for the store.
  set_variable(bank_.password());
  EXPECT_EQ(with_profiles({"account"}).code, elctl::kOk);
  // A profile that is not there is said.
  const Outcome missing = with_profiles({"account", "--profile", "other"});
  EXPECT_EQ(missing.code, elctl::kUsage);
  EXPECT_TRUE(contains(missing.err, "no profile 'other'")) << missing.err;
}

// A keyring that cannot be opened (locked, no session) does not stop a script
// that gives the password in the variable.
TEST_F(ElctlEarthbankAccount, AStoreThatFailsDoesNotBlockTheVariable) {
  class Broken final : public pychron::earthbank::ISecretStore {
   public:
    std::string backend() const override { return "broken"; }
    bool persistent() const override { return true; }
    pychron::Result<std::optional<std::string>> get(std::string_view) override {
      return pychron::fail(pychron::ErrorKind::Io, "broken: the keyring is locked");
    }
    pychron::Result<void> set(std::string_view, std::string_view) override { return {}; }
    pychron::Result<void> erase(std::string_view) override { return {}; }
  } broken;
  ASSERT_EQ(log_in().code, elctl::kOk);
  const auto run = [&](std::vector<std::string> args) {
    std::istringstream in;
    std::ostringstream out, err;
    args.insert(args.end(), {"--profiles", profiles_.string()});
    Outcome o;
    o.code = elctl::earthbank_command(args, elctl::Io{in, out, err}, elctl::EarthbankDeps{&spy_, &broken});
    o.out = out.str();
    o.err = err.str();
    return o;
  };
  set_variable(bank_.password());
  EXPECT_EQ(run({"account"}).code, elctl::kOk);
  EXPECT_EQ(run({"packages"}).code, elctl::kOk);
  // With no variable the store's own words are the error.
  set_variable(std::nullopt);
  const Outcome none = run({"account"});
  EXPECT_EQ(none.code, elctl::kUsage);
  EXPECT_TRUE(contains(none.err, "the keyring is locked")) << none.err;
  // A pre-flight goes on, unsigned, and says what the store said.
  const Outcome pre = run({"preflight", "--db", db_});
  EXPECT_TRUE(contains(pre.err, "the keyring is locked")) << pre.err;
  EXPECT_TRUE(contains(pre.out, "not signed in")) << pre.out;
}

TEST_F(ElctlEarthbankAccount, PackagesListsAndCreates) {
  ASSERT_EQ(log_in().code, elctl::kOk);
  const Outcome list = with_profiles({"packages"});
  ASSERT_EQ(list.code, elctl::kOk) << list.err;
  EXPECT_TRUE(contains(list.out, "610001  Example Lab Ar-Ar 2026")) << list.out;
  EXPECT_TRUE(contains(list.out, "data packages")) << list.out;
  EXPECT_TRUE(contains(list.out, "institution 9301")) << list.out;  // the profile's

  const Outcome made = with_profiles({"packages", "--create", "Ar-Ar results 2031", "--institution", "9301"});
  ASSERT_EQ(made.code, elctl::kOk) << made.err;
  EXPECT_TRUE(contains(made.out, "created data package ")) << made.out;
  EXPECT_TRUE(contains(made.out, "'Ar-Ar results 2031'")) << made.out;
  const auto store = bank_.store();
  ASSERT_FALSE(store.packages.empty());
  const auto& row = store.packages.back().at("dataPackageDTO");
  EXPECT_EQ(row.at("name"), "Ar-Ar results 2031");
  const std::int64_t id = row.at("id").get<std::int64_t>();
  EXPECT_TRUE(contains(made.out, std::to_string(id))) << made.out;

  // The profile uploads to it from now on, and the list marks it.
  const auto profiles = pychron::earthbank::load_profiles(profiles_);
  ASSERT_TRUE(profiles);
  EXPECT_EQ(profiles->find("mylab")->data_package_id, std::optional<std::int64_t>(id));
  EXPECT_EQ(profiles->find("mylab")->institution_id, std::optional<std::int64_t>(9301));
  const Outcome marked = with_profiles({"packages"});
  EXPECT_TRUE(contains(marked.out, "* " + std::to_string(id) + "  Ar-Ar results 2031")) << marked.out;

  // A package the service made under another name is the service's word.
  bank_.fail_next("POST", "/api/management/data-packages", 201,
                  R"({"id":9999,"name":"Another name"})");
  const Outcome misnamed = with_profiles({"packages", "--create", "Asked for"});
  EXPECT_EQ(misnamed.code, elctl::kFailed);
  EXPECT_TRUE(contains(misnamed.err, "Another name")) << misnamed.err;
  EXPECT_EQ(pychron::earthbank::load_profiles(profiles_)->find("mylab")->data_package_id, std::optional<std::int64_t>(id));
  EXPECT_EQ(with_profiles({"packages", "--institution", "x"}).code, elctl::kUsage);
}

TEST_F(ElctlEarthbankAccount, PreflightListsMissesAndExitsOne) {
  ASSERT_EQ(log_in().code, elctl::kOk);
  const std::string file = overrides("[samples.\"FC-2\"]\ninterpretation = \"No Such Interpretation\"\n");
  const Outcome o = with_profiles({"preflight", "--db", db_, "--overrides", file});
  EXPECT_EQ(o.code, elctl::kFailed) << o.err << o.out;
  EXPECT_TRUE(contains(o.out, "signed in to https://earthbank.test as profile 'mylab'")) << o.out;
  // The sample is a heading, and what is wrong with it is under it.
  EXPECT_TRUE(contains(o.out, "\nFC-2\n  ArArAgeSummary.interpretationName: 'No Such Interpretation' is not known to EarthBank"))
      << o.out;
  EXPECT_FALSE(contains(o.out, "\nS-FUS\n  ArArAgeSummary.interpretationName")) << o.out;
  EXPECT_TRUE(contains(o.out, " not known to EarthBank; ")) << o.out;
  // Nothing was written to the service: a pre-flight only reads.
  for (const auto& r : bank_.requests()) {
    if (r.path == "/api/authenticate") continue;
    EXPECT_EQ(r.method, "GET") << r;
  }
  // A problem in the selection is as for the workbooks.
  EXPECT_EQ(with_profiles({"preflight", "--db", db_, "--sample", "nobody"}).code, elctl::kFailed);
  EXPECT_EQ(with_profiles({"preflight"}).code, elctl::kUsage);
  EXPECT_EQ(with_profiles({"preflight", "--db", db_, "--frob", "x"}).code, elctl::kUsage);
}

TEST_F(ElctlEarthbankAccount, PreflightWithoutALoginSaysWhatItCouldNotAsk) {
  // No profile, no password: the lists are asked as they open to anyone, and
  // those that answer that they want a login are said not to have been asked
  // (here the flux monitors do: no list of the fixture wants a login for the
  // terms the seeded samples have).
  bank_.fail_next("GET", "/api/arar/FluxMonitor", 401,
                  R"({"title":"Unauthorized","status":401,"detail":"Full authentication is required"})");
  const Outcome o = with_profiles({"preflight", "--db", db_, "--base-url", bank_.base_url()});
  EXPECT_EQ(o.code, elctl::kFailed) << o.err << o.out;
  EXPECT_TRUE(contains(o.out, "not signed in to https://earthbank.test: there is no profile to sign in with")) << o.out;
  EXPECT_TRUE(contains(o.out, "\nFC-2\n  ArArAgeCalc.fluxMonitorName: 'FC-2 28.201 Ma' could not be asked (/api/arar/FluxMonitor)")) << o.out;
  EXPECT_TRUE(contains(o.out, "0 terms are not known to EarthBank; 2 terms could not be asked about")) << o.out;
  EXPECT_FALSE(bank_.requests().empty());
  for (const auto& r : bank_.requests()) {
    EXPECT_EQ(r.method, "GET") << r;
    EXPECT_FALSE(r.authenticated) << r;
  }
}

TEST_F(ElctlEarthbankAccount, ImportProfiles) {
  const fs::path ini = path("pychron.ausgeochem.ini");
  std::ofstream(ini) << "[pychron.ausgeochem]\n"
                        "profiles_json = '[{\"name\": \"default\", \"base_url\": \"https://earthbank.test\", "
                        "\"username\": \"r.fixture\", \"data_package_id\": \"4321\", \"institution_id\": \"1001\", "
                        "\"password\": \"not-a-real-password\"}, "
                        "{\"name\": \"other\", \"base_url\": \"https://earthbank.test\", \"username\": \"s.sample\"}]'\n"
                        "active_profile = other\n";
  const Outcome o = with_profiles({"import-profiles", ini.string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "imported 2 profiles")) << o.out;
  EXPECT_TRUE(contains(o.out, "passwords are not imported")) << o.out;
  const auto profiles = pychron::earthbank::load_profiles(profiles_);
  ASSERT_TRUE(profiles) << profiles.error().what;
  ASSERT_EQ(profiles->profiles.size(), 2u);
  EXPECT_EQ(profiles->active, "other");
  EXPECT_EQ(profiles->find("default")->data_package_id, std::optional<std::int64_t>(4321));
  EXPECT_FALSE(contains(slurp(profiles_), "not-a-real-password"));
  EXPECT_FALSE(contains(o.out + o.err, "not-a-real-password"));

  // Again: what is there is the user's, and is said.
  const Outcome twice = with_profiles({"import-profiles", ini.string()});
  ASSERT_EQ(twice.code, elctl::kOk) << twice.err;
  EXPECT_TRUE(contains(twice.out, "imported 0 profiles")) << twice.out;
  EXPECT_TRUE(contains(twice.out, "profile 'default' is already in")) << twice.out;
  EXPECT_EQ(with_profiles({"import-profiles", path("nothing.ini").string()}).code, elctl::kUsage);
  EXPECT_EQ(with_profiles({"import-profiles"}).code, elctl::kUsage);
}

// Whatever a command printed, a password and a token are not in it: not on a
// success, not on a failure.
TEST_F(ElctlEarthbankAccount, NoPasswordOrTokenInAnyOutput) {
  ASSERT_EQ(log_in().code, elctl::kOk);
  with_profiles({"account"});
  with_profiles({"packages"});
  with_profiles({"packages", "--create", "Another one"});
  with_profiles({"preflight", "--db", db_});
  bank_.expire_tokens();
  with_profiles({"account"});  // a 401, and a new login
  bank_.fail_next("GET", "/api/account", 500, R"({"detail":"said of )" + bank_.password() + R"("})");
  with_profiles({"account"});
  bank_.set_password("a-changed-password-8812");
  with_profiles({"account"});  // the stored password is refused
  set_variable("a-wrong-password-4417");
  with_profiles({"login", "--profile", "mylab"});
  set_variable("a-changed-password-8812");
  with_profiles({"login", "--profile", "mylab"});

  ASSERT_FALSE(spy_.secrets().empty());
  std::vector<std::string> secrets = spy_.secrets();
  secrets.insert(secrets.end(), {"not-a-real-password", "a-wrong-password-4417", "a-changed-password-8812", "Bearer"});
  for (const std::string& output : outputs_) {
    for (const std::string& secret : secrets) EXPECT_FALSE(contains(output, secret)) << secret << " in " << output;
  }
  // Nor in the profiles file.
  for (const std::string& secret : secrets) EXPECT_FALSE(contains(slurp(profiles_), secret)) << secret;
}

TEST_F(ElctlEarthbankAccount, HelpListsTheAccountCommands) {
  for (const char* sub : {"login", "account", "packages", "preflight", "upload", "import-profiles"}) {
    const Outcome help = run_raw({"earthbank", sub, "--help"});
    EXPECT_EQ(help.code, elctl::kOk) << sub;
    EXPECT_TRUE(contains(help.out, std::string("elctl earthbank ") + sub)) << sub << help.out;
    EXPECT_TRUE(contains(help.out, "EARTHBANK_PASSWORD")) << help.out;
  }
  EXPECT_TRUE(contains(run_raw({"help"}).out, "earthbank login|account|packages|preflight|upload|import-profiles"));
}

#ifndef PYCHRON_ELCTL_HAS_NETWORK
// A build with persistence and no Qt6 Network: the commands that need the
// network say so and exit 2, before they ask for anything.
TEST_F(ElctlEarthbankAccount, WithoutNetworkSupportTheNetworkCommandsSayNo) {
  for (const char* sub : {"login", "account", "packages", "preflight", "upload"}) {
    std::istringstream in;
    std::ostringstream out, err;
    const int code = elctl::earthbank_command({sub, "--profiles", profiles_.string()}, elctl::Io{in, out, err});
    EXPECT_EQ(code, elctl::kUsage) << sub;
    EXPECT_TRUE(contains(err.str(), "this build has no network support (Qt6 Network was not found)")) << err.str();
    EXPECT_EQ(out.str(), "");
  }
}
#endif

// ---------------------------------------------------------------- upload

// `elctl earthbank upload` against the fake: signed in as `mylab`, into the
// fixture's writable package, with the journals in a directory of the test's.
// Whatever any of these tests printed is searched for a password or a token
// when the test ends (the plan's NoPasswordOrTokenInAnyOutput, over every
// upload test), and so are the journals.
class ElctlEarthbankUpload : public ElctlEarthbankAccount {
 protected:
  void SetUp() override {
    ElctlEarthbankAccount::SetUp();
    journal_ = path("journal");
    package_ = bank_.writable_package();
    ASSERT_EQ(log_in().code, elctl::kOk);
  }

  void TearDown() override {
    std::vector<std::string> secrets = spy_.secrets();
    EXPECT_FALSE(secrets.empty());
    secrets.insert(secrets.end(), {"not-a-real-password", "a-changed-password-8812", "Bearer"});
    std::vector<std::string> texts = outputs_;
    std::error_code code;
    if (fs::exists(journal_, code)) {
      for (const auto& entry : fs::recursive_directory_iterator(journal_))
        if (entry.is_regular_file()) texts.push_back(slurp(entry.path()));
    }
    for (const std::string& text : texts) {
      for (const std::string& secret : secrets) EXPECT_FALSE(contains(text, secret)) << secret << " in " << text;
    }
    ElctlEarthbankAccount::TearDown();
  }

  std::vector<std::string> upload_args(const std::vector<std::string>& more = {}) const {
    std::vector<std::string> args{"upload", "--db", db_, "--package", std::to_string(package_), "--journal", journal_.string()};
    args.insert(args.end(), more.begin(), more.end());
    return args;
  }
  Outcome upload(const std::vector<std::string>& more = {}) { return with_profiles(upload_args(more)); }

  // The records written to the service: every POST but a login.
  std::size_t posts() const { return bank_.count("POST", "/api/core") + bank_.count("POST", "/api/arar"); }

  std::size_t journals() const {
    std::size_t n = 0;
    std::error_code code;
    if (!fs::exists(journal_, code)) return 0;
    for (const auto& entry : fs::recursive_directory_iterator(journal_))
      if (entry.is_regular_file() && entry.path().extension() == ".json") ++n;
    return n;
  }

  // The words of a command line the program printed, as a shell would take
  // them: single quotes kept together, `'\''` a quote.
  static std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::string word;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (quoted) {
        if (c == '\'') {
          quoted = false;
        } else {
          word += c;
        }
      } else if (c == '\'') {
        quoted = any = true;
      } else if (c == '\\' && i + 1 < line.size()) {
        word += line[++i];
        any = true;
      } else if (c == ' ') {
        if (any) out.push_back(word);
        word.clear();
        any = false;
      } else {
        word += c;
        any = true;
      }
    }
    if (any) out.push_back(word);
    return out;
  }

  // The command lines `text` offers (the lines that begin, indented, with
  // `elctl earthbank `), each as the arguments of `earthbank`.
  static std::vector<std::vector<std::string>> offered(const std::string& text) {
    std::vector<std::vector<std::string>> out;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
      const std::string start = "    elctl earthbank ";
      if (!line.starts_with(start)) continue;
      out.push_back(words(line.substr(start.size())));
    }
    return out;
  }

  fs::path journal_;
  std::int64_t package_ = 0;
};

TEST_F(ElctlEarthbankUpload, UploadWithoutYesSendsNothingAndSaysWhatItWould) {
  const Outcome o = upload();
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  // Who it would be sent as, and where.
  EXPECT_TRUE(contains(o.out, "(" + bank_.login() + ")")) << o.out;
  EXPECT_TRUE(contains(o.out, "data package: " + std::to_string(package_) + " '")) << o.out;
  // What, by sample: the analyses and the age, then the records.
  EXPECT_TRUE(contains(o.out, "FC-2: 2 analyses, ")) << o.out;
  EXPECT_TRUE(contains(o.out, "S-FUS: 2 analyses, ")) << o.out;
  EXPECT_TRUE(contains(o.out, "  to write: the sample if the package has none of that name, 1 data point, 1 aliquot, "
                              "2 measurements, 1 age calculation, 1 age summary\n"))
      << o.out;
  EXPECT_TRUE(contains(o.out, "every term the upload would send is known to EarthBank")) << o.out;
  EXPECT_TRUE(contains(o.err, "nothing was sent; add --yes to upload")) << o.err;
  EXPECT_EQ(posts(), 0u);
  EXPECT_EQ(journals(), 0u);
}

TEST_F(ElctlEarthbankUpload, UploadWritesAndPrintsTheIds) {
  const Outcome o = upload({"--yes"});
  ASSERT_EQ(o.code, elctl::kOk) << o.err << o.out;
  const auto store = bank_.store();
  ASSERT_EQ(store.samples.size(), 2u);
  ASSERT_EQ(store.data_points.size(), 2u);
  EXPECT_EQ(store.measurements.size(), 4u);
  EXPECT_EQ(store.age_calcs.size(), 2u);
  EXPECT_EQ(store.age_summaries.size(), 2u);
  for (const auto& point : store.data_points) EXPECT_EQ(point.package_id, package_);
  // A line for each record as it is written, then each sample's outcome.
  EXPECT_TRUE(contains(o.out, "FC-2: data point: saved, id " + std::to_string(store.data_points[0].id) + "\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "FC-2: age summary: saved, id ")) << o.out;
  EXPECT_TRUE(contains(o.out, "FC-2: complete, data point " + std::to_string(store.data_points[0].id) + "\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "S-FUS: complete, data point " + std::to_string(store.data_points[1].id) + "\n")) << o.out;
  EXPECT_EQ(journals(), 2u);

  // Again: it is all there, and nothing is sent twice.
  const std::size_t sent = posts();
  const Outcome again = upload({"--yes"});
  EXPECT_EQ(again.code, elctl::kOk) << again.err << again.out;
  EXPECT_EQ(posts(), sent);
  EXPECT_TRUE(contains(again.out, "FC-2: complete, data point " + std::to_string(store.data_points[0].id) + "\n")) << again.out;
}

TEST_F(ElctlEarthbankUpload, UploadStopsOnMissesWithExitOne) {
  const std::string file = overrides("[samples.\"FC-2\"]\ninterpretation = \"No Such Interpretation\"\n");
  for (const bool yes : {false, true}) {
    std::vector<std::string> more{"--overrides", file};
    if (yes) more.emplace_back("--yes");
    const Outcome o = upload(more);
    EXPECT_EQ(o.code, elctl::kFailed) << o.err << o.out;
    EXPECT_TRUE(contains(o.out, "\nFC-2\n  ArArAgeSummary.interpretationName: 'No Such Interpretation' is not known to EarthBank"))
        << o.out;
    EXPECT_TRUE(contains(o.err, "nothing was sent")) << o.err;
    EXPECT_TRUE(contains(o.err, "--allow-unresolved")) << o.err;
    EXPECT_EQ(posts(), 0u);
    EXPECT_EQ(journals(), 0u);
  }
}

TEST_F(ElctlEarthbankUpload, AllowUnresolvedProceeds) {
  const std::string file = overrides("[samples.\"FC-2\"]\ninterpretation = \"No Such Interpretation\"\n");
  const Outcome o = upload({"--overrides", file, "--allow-unresolved", "--yes"});
  ASSERT_EQ(o.code, elctl::kOk) << o.err << o.out;
  const auto store = bank_.store();
  ASSERT_EQ(store.age_summaries.size(), 2u);
  // The term is not sent, by name or by id, and the sample says so.
  for (const auto& summary : store.age_summaries) EXPECT_FALSE(contains(summary.body.dump(), "No Such Interpretation"));
  EXPECT_TRUE(contains(o.out, "No Such Interpretation")) << o.out;
  EXPECT_TRUE(contains(o.out, "FC-2: complete, data point ")) << o.out;
}

TEST_F(ElctlEarthbankUpload, UploadWithNoPackageSaysHowToSetOne) {
  // The profile has no package and none is given.
  const Outcome o = with_profiles({"upload", "--db", db_, "--journal", journal_.string(), "--yes"});
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  EXPECT_TRUE(contains(o.err, "--package <id>")) << o.err;
  EXPECT_TRUE(contains(o.err, "elctl earthbank packages --create <name>")) << o.err;
  EXPECT_EQ(posts(), 0u);

  // A package the profile has is the one used.
  ASSERT_EQ(with_profiles({"packages", "--create", "Mine"}).code, elctl::kOk);
  const std::size_t before = posts();
  const Outcome mine = with_profiles({"upload", "--db", db_, "--journal", journal_.string(), "--sample", "FC-2", "--yes"});
  ASSERT_EQ(mine.code, elctl::kOk) << mine.err << mine.out;
  EXPECT_TRUE(contains(mine.out, "'Mine'")) << mine.out;
  EXPECT_GT(posts(), before);
  ASSERT_EQ(bank_.store().data_points.size(), 1u);
  EXPECT_NE(bank_.store().data_points[0].package_id, package_);
}

TEST_F(ElctlEarthbankUpload, APartialUploadPrintsTheCommandToFinish) {
  // The age summary is refused: the data point is there and one record is not.
  bank_.fail_next("POST", "/api/arar/ArArAgeSummary", 400, R"({"title":"Bad Request","status":400,"detail":"not today"})");
  const Outcome o = upload({"--sample", "FC-2", "--yes"});
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  ASSERT_EQ(bank_.store().data_points.size(), 1u);
  EXPECT_TRUE(bank_.store().age_summaries.empty());
  EXPECT_TRUE(contains(o.out, "FC-2: partial, data point " + std::to_string(bank_.store().data_points[0].id) + "\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "age summary")) << o.out;
  const auto commands = offered(o.out);
  ASSERT_EQ(commands.size(), 1u) << o.out;
  // It is this command again, word for word: the journal knows the rest.
  std::vector<std::string> expected = upload_args({"--sample", "FC-2", "--yes"});
  expected.insert(expected.end(), {"--profiles", profiles_.string()});
  EXPECT_EQ(commands[0], expected);

  const Outcome finished = eb(commands[0]);
  ASSERT_EQ(finished.code, elctl::kOk) << finished.err << finished.out;
  const auto store = bank_.store();
  EXPECT_EQ(store.samples.size(), 1u);
  EXPECT_EQ(store.data_points.size(), 1u);
  EXPECT_EQ(store.aliquots.size(), 1u);
  EXPECT_EQ(store.measurements.size(), 2u);
  EXPECT_EQ(store.age_calcs.size(), 1u);
  EXPECT_EQ(store.age_summaries.size(), 1u);
  EXPECT_TRUE(contains(finished.out, "FC-2: age summary: saved, id ")) << finished.out;
  EXPECT_FALSE(contains(finished.out, "FC-2: data point: saved")) << finished.out;
}

TEST_F(ElctlEarthbankUpload, InDoubtPrintsTheTwoWaysToContinue) {
  // The age calculation is written and its answer lost.
  bank_.drop_next("POST", "/api/arar/ArArAgeCalc");
  const Outcome o = upload({"--sample", "FC-2", "--yes"});
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  EXPECT_TRUE(contains(o.out, "FC-2: in doubt, data point ")) << o.out;
  EXPECT_TRUE(contains(o.out, "the age calculation may or may not have been saved on EarthBank")) << o.out;
  // The library's own word for the setting is not what the user types.
  EXPECT_FALSE(contains(o.out, "resolve_in_doubt")) << o.out;
  const auto commands = offered(o.out);
  ASSERT_EQ(commands.size(), 2u) << o.out;
  std::vector<std::string> base = upload_args({"--sample", "FC-2", "--yes"});
  base.insert(base.end(), {"--profiles", profiles_.string()});
  std::vector<std::string> written = base, not_written = base;
  written.insert(written.end(), {"--in-doubt", "written:<id>"});
  not_written.insert(not_written.end(), {"--in-doubt", "not-written"});
  EXPECT_EQ(commands[0], written);
  EXPECT_EQ(commands[1], not_written);

  // Run again as it was: it stops at the same place, and sends nothing.
  const std::size_t sent = posts();
  const Outcome same = upload({"--sample", "FC-2", "--yes"});
  EXPECT_EQ(same.code, elctl::kUsage);
  EXPECT_EQ(posts(), sent);
  EXPECT_EQ(offered(same.out).size(), 2u) << same.out;

  // The user looked: it is there. The first way, with its id, finishes it;
  // the command that is printed then does not carry the answer on.
  ASSERT_EQ(bank_.store().age_calcs.size(), 1u);
  std::vector<std::string> answer = commands[0];
  answer.back() = "written:" + std::to_string(bank_.store().age_calcs[0].id);
  const Outcome finished = eb(answer);
  ASSERT_EQ(finished.code, elctl::kOk) << finished.err << finished.out;
  EXPECT_EQ(bank_.store().age_calcs.size(), 1u);
  EXPECT_EQ(bank_.store().age_summaries.size(), 1u);
  EXPECT_TRUE(offered(finished.out).empty()) << finished.out;

  // A word that is neither way is refused before anything is asked.
  const std::size_t requests = bank_.requests().size();
  EXPECT_EQ(upload({"--yes", "--in-doubt", "maybe"}).code, elctl::kUsage);
  EXPECT_EQ(upload({"--yes", "--in-doubt", "written:x"}).code, elctl::kUsage);
  EXPECT_EQ(bank_.requests().size(), requests);
}

TEST_F(ElctlEarthbankUpload, AChangedSubmissionPrintsTheCommandThatSendsItAsNew) {
  ASSERT_EQ(upload({"--sample", "FC-2", "--yes"}).code, elctl::kOk);
  const std::size_t sent = posts();
  const std::string file = overrides("[samples.\"FC-2\"]\ndatapoint_name = \"FC-2 again\"\n");
  const Outcome o = upload({"--sample", "FC-2", "--overrides", file, "--yes"});
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  EXPECT_EQ(posts(), sent);
  EXPECT_TRUE(contains(o.out, "FC-2: refused\n")) << o.out;
  EXPECT_FALSE(contains(o.out, "new_datapoint")) << o.out;
  const auto commands = offered(o.out);
  ASSERT_EQ(commands.size(), 1u) << o.out;
  EXPECT_EQ(commands[0].back(), "--new-datapoint");
  const Outcome fresh = eb(commands[0]);
  ASSERT_EQ(fresh.code, elctl::kOk) << fresh.err << fresh.out;
  EXPECT_EQ(bank_.store().data_points.size(), 2u);
}

TEST_F(ElctlEarthbankUpload, NoStoredPasswordAndNoTerminalExitsTwoHavingSentNothing) {
  const auto profiles = pychron::earthbank::load_profiles(profiles_);
  ASSERT_TRUE(profiles);
  ASSERT_TRUE(secrets_->erase(pychron::earthbank::secret_account(*profiles->find("mylab"))));
  const std::size_t requests = bank_.requests().size();
  const Outcome o = upload({"--yes"});
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  EXPECT_TRUE(contains(o.err, "no password is kept for profile 'mylab' and $EARTHBANK_PASSWORD is not set")) << o.err;
  EXPECT_TRUE(contains(o.err, "elctl earthbank login")) << o.err;
  EXPECT_EQ(bank_.requests().size(), requests);  // not even a question
  EXPECT_EQ(journals(), 0u);

  // The variable is enough for a script.
  set_variable(bank_.password());
  const Outcome with_variable = upload({"--sample", "FC-2", "--yes"});
  set_variable(std::nullopt);
  EXPECT_EQ(with_variable.code, elctl::kOk) << with_variable.err << with_variable.out;
}

TEST_F(ElctlEarthbankUpload, AWrongStoredPasswordSaysToLoginAgain) {
  bank_.set_password("a-changed-password-8812");
  const Outcome o = upload({"--yes"});
  EXPECT_EQ(o.code, elctl::kUsage) << o.err << o.out;
  EXPECT_TRUE(contains(o.err, "elctl earthbank login")) << o.err;
  EXPECT_EQ(posts(), 0u);
  EXPECT_EQ(journals(), 0u);
}

TEST_F(ElctlEarthbankUpload, UsageAndSelectionErrors) {
  const std::size_t requests = bank_.requests().size();
  EXPECT_EQ(with_profiles({"upload", "--yes"}).code, elctl::kUsage);  // no --db
  EXPECT_EQ(upload({"--frob", "x"}).code, elctl::kUsage);
  EXPECT_EQ(upload({"--package", "none"}).code, elctl::kUsage);
  EXPECT_EQ(upload({"--journal", ""}).code, elctl::kUsage);
  EXPECT_EQ(bank_.requests().size(), requests);
  // Nothing matched: as for the workbooks.
  EXPECT_EQ(upload({"--sample", "nobody", "--yes"}).code, elctl::kFailed);
  EXPECT_EQ(posts(), 0u);
}

}  // namespace

#endif
