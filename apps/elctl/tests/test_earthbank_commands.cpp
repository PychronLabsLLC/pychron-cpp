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

#include "ooxml.hpp"
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
flux_monitor = "Fish Canyon Tuff sanidine Kuiper et al. 2008"
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
  EXPECT_NE(at(calcs[0], "fluxMonitor"), "Fish Canyon Tuff sanidine Kuiper et al. 2008");
  EXPECT_EQ(at(calcs[1], "fluxMonitor"), "Fish Canyon Tuff sanidine Kuiper et al. 2008");

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
    if (check) args.push_back("--check");
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
    if (check) args.push_back("--check");
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

}  // namespace

#endif
