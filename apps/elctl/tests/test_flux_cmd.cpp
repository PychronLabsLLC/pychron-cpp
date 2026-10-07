// elctl flux fit: a level's J fitted from its monitors, driven through
// elctl::run against a file-backed SQLite store holding the seeded level
// NM-300 A of tests/processing/flux_seed.hpp.

#include <gtest/gtest.h>

#include "elctl_fixture.hpp"
#include "flux.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_HAS_STORE

TEST(FluxCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"flux", "fit", "NM-300", "A", "--db", "sqlite::memory:"});
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

#include "flux_seed.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/store.hpp"

namespace {

namespace ps = pychron::persistence;
namespace pt = pychron::processing::testing;
namespace fs = std::filesystem;

std::vector<std::string> split_ws(const std::string& line) {
  std::istringstream in(line);
  std::vector<std::string> tokens;
  for (std::string t; in >> t;) tokens.push_back(t);
  return tokens;
}

// The cells of the row of `hole` in the table headed `section` ("Monitors", "Unknowns").
std::vector<std::string> table_row(const std::string& out, const std::string& section, int hole) {
  std::istringstream in(out);
  bool inside = false;
  for (std::string line; std::getline(in, line);) {
    if (line == section) {
      inside = true;
      continue;
    }
    if (!inside) continue;
    if (line.empty() || line == "Monitors" || line == "Unknowns") break;
    const auto cells = split_ws(line);
    if (!cells.empty() && cells[0] == std::to_string(hole)) return cells;
  }
  return {};
}

// RFC 4180: quoted fields hold commas, quotes ("") and line breaks.
std::vector<std::vector<std::string>> parse_csv(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false, any = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        field += '"';
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        field += c;
      }
    } else if (c == '"') {
      quoted = true;
      any = true;
    } else if (c == ',') {
      row.push_back(field);
      field.clear();
      any = true;
    } else if (c == '\r' || c == '\n') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      row.push_back(field);
      rows.push_back(row);
      row.clear();
      field.clear();
      any = false;
    } else {
      field += c;
      any = true;
    }
  }
  if (any) {
    row.push_back(field);
    rows.push_back(row);
  }
  return rows;
}

class FluxCmd : public elctl::testing::ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    db_ = make_store("store.db", "FC-2");
  }

  void TearDown() override {
    store_.reset();
    ElctlTest::TearDown();
  }

  // A fresh store holding the seeded level; the first one made is kept open.
  std::string make_store(const std::string& file, const std::string& monitor_sample) {
    const std::string url = "sqlite:" + path(file).string();
    auto store = ps::open_store(ps::StoreConfig{url, true});
    EXPECT_TRUE(store) << (store ? "" : to_string(store.error()));
    auto client = (*store)->register_client({"red-1", "reduction", std::nullopt, "test"});
    auto user = (*store)->ensure_user(*client, "jsmith");
    auto seeded = pt::seed_flux_level(**store, ps::Actor{*user, *client}, monitor_sample);
    EXPECT_TRUE(seeded) << (seeded ? "" : to_string(seeded.error()));
    seeded_ = *seeded;
    if (!store_) {
      store_ = std::move(*store);
      actor_ = ps::Actor{*user, *client};
    }
    return url;
  }

  Outcome fit(std::vector<std::string> args, const std::string& db = "") const {
    std::vector<std::string> all{"flux", "fit", "NM-300", "A", "--db", db.empty() ? db_ : db};
    all.insert(all.end(), args.begin(), args.end());
    return run_raw(std::move(all));
  }

  ps::ChangeSeq seq() const { return *store_->latest_change_seq(); }

  std::string db_;
  std::unique_ptr<ps::IStore> store_;
  pt::SeededLevel seeded_;
  ps::Actor actor_;
};

TEST_F(FluxCmd, PrintsBothTablesAndWritesNothing) {
  const auto before = seq();
  const Outcome o = fit({});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "NM-300 A")) << o.out;
  EXPECT_TRUE(contains(o.out, "FC-2 (Kuiper 2008): 28.201 +/- 0.046 Ma")) << o.out;
  EXPECT_TRUE(contains(o.out, "Monitors")) << o.out;
  EXPECT_TRUE(contains(o.out, "Unknowns")) << o.out;
  // The columns are padded to their widest cell: compare with the spaces collapsed.
  std::string collapsed;
  for (const char c : o.out) {
    if (c == ' ' && !collapsed.empty() && collapsed.back() == ' ') continue;
    collapsed += c;
  }
  EXPECT_TRUE(contains(collapsed, "hole identifier sample n saved J +/- mean J +/- % MSWD pred J +/- % dev % fit")) << o.out;
  EXPECT_TRUE(contains(collapsed, "hole identifier sample saved J +/- pred J +/- % dev %\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "66001")) << o.out;
  EXPECT_TRUE(contains(o.out, "66101")) << o.out;
  EXPECT_TRUE(contains(o.out, "model plane, unweighted; mean arithmetic (msem); fit error msem")) << o.out;
  EXPECT_TRUE(contains(o.out, "fit MSWD")) << o.out;
  EXPECT_EQ(seq(), before);
  // A monitor row: three analyses, in the fit, nothing saved yet.
  const auto row = table_row(o.out, "Monitors", 1);
  ASSERT_EQ(row.size(), 15u) << o.out;
  EXPECT_EQ(row[1], "66001");
  EXPECT_EQ(row[3], "3");
  EXPECT_EQ(row[4], "-");
  EXPECT_EQ(row.back(), "yes");
  // An unknown row: no saved J, a predicted J.
  const auto unknown = table_row(o.out, "Unknowns", 9);
  ASSERT_EQ(unknown.size(), 9u) << o.out;
  EXPECT_EQ(unknown[3], "-");
  EXPECT_NE(unknown[5], "-");
}

TEST_F(FluxCmd, SaveThenRepeatSaysUnchanged) {
  const auto before = seq();
  Outcome o = fit({"--save", "--user", "jsmith"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "saved 12 positions (0 unchanged)")) << o.out;
  EXPECT_GT(seq(), before);
  const auto after = seq();
  o = fit({"--save", "--user", "jsmith"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "nothing to save: 12 positions unchanged")) << o.out;
  EXPECT_EQ(seq(), after);
  // The saved J now shows, and agrees with the prediction.
  const auto row = table_row(o.out, "Monitors", 1);
  ASSERT_EQ(row.size(), 15u) << o.out;
  EXPECT_NE(row[4], "-");
}

TEST_F(FluxCmd, OptionsComeFromTheSavedFit) {
  ASSERT_EQ(fit({"--model", "plane", "--weighted", "--save"}).code, elctl::kOk);
  Outcome o = fit({});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "model plane, weighted")) << o.out;
  // A model flag replaces the model and keeps the rest.
  o = fit({"--model", "nearest"});
  EXPECT_TRUE(contains(o.out, "model nearest")) << o.out;
  EXPECT_TRUE(contains(o.out, "mean arithmetic (msem)")) << o.out;
  // A mean model's own options are saved too.
  ASSERT_EQ(fit({"--model", "nearest", "--neighbors", "3", "--save"}).code, elctl::kOk);
  o = fit({});
  EXPECT_TRUE(contains(o.out, "model nearest, 3 neighbors")) << o.out;
}

TEST_F(FluxCmd, FlagsReplaceOneFieldOfTheSavedFit) {
  ASSERT_EQ(fit({"--model", "plane", "--weighted", "--save"}).code, elctl::kOk);
  Outcome o = fit({"--unweighted"});
  EXPECT_TRUE(contains(o.out, "model plane, unweighted; mean arithmetic (msem)")) << o.out;
  o = fit({"--mean", "weighted"});
  EXPECT_TRUE(contains(o.out, "model plane, weighted; mean weighted (msem)")) << o.out;
  o = fit({"--mean-error", "sem"});
  EXPECT_TRUE(contains(o.out, "model plane, weighted; mean arithmetic (sem); fit error msem")) << o.out;
  o = fit({"--fit-error", "sem"});
  EXPECT_TRUE(contains(o.out, "model plane, weighted; mean arithmetic (msem); fit error sem")) << o.out;
}

TEST_F(FluxCmd, ASavedLeastSquaresFitWithSdWarnsAndUsesMsem) {
  ps::FluxValue v;
  v.j = 0.001;
  v.j_err = 1e-6;
  v.options_json = R"({"model_kind":"Plane","predicted_j_error_type":"SD","error_kind":"MSEM"})";
  ASSERT_TRUE(pt::seed_save_flux(*store_, actor_, seeded_, 1, v));
  const std::string warning = "warning: saved fit used SD, which a fitted surface does not have: using msem";
  Outcome o = fit({});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, warning)) << o.out;
  EXPECT_TRUE(contains(o.out, "fit error msem")) << o.out;
  // A model flag does not hide it while the fit error is the saved one and the model a surface.
  o = fit({"--model", "plane", "--weighted"});
  EXPECT_TRUE(contains(o.out, warning)) << o.out;
  // Where the model is not a surface the saved fit error is not in question.
  o = fit({"--model", "nearest"});
  EXPECT_FALSE(contains(o.out, warning)) << o.out;
  // An explicit --fit-error replaces the saved one.
  o = fit({"--fit-error", "sem"});
  EXPECT_FALSE(contains(o.out, warning)) << o.out;
  EXPECT_TRUE(contains(o.out, "fit error sem")) << o.out;
}

// R17: the monitor standard does not change without a word.
TEST_F(FluxCmd, ASavedMonitorSetTheStoreLacksWarns) {
  ps::FluxValue v;
  v.j = 0.001;
  v.j_err = 1e-6;
  v.options_json = R"({"model_kind":"Plane","monitor_reference":"FC Min"})";  // as imported
  ASSERT_TRUE(pt::seed_save_flux(*store_, actor_, seeded_, 1, v));
  const std::string warning =
      "warning: saved fit used monitor set 'FC Min', which the store does not have: using 'FC-2 (Kuiper 2008)'\n";
  Outcome o = fit({});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, warning)) << o.out;
  EXPECT_TRUE(contains(o.out, "monitors FC-2 (Kuiper 2008):")) << o.out;
  // The user names the set: nothing to warn of.
  for (const char* named : {"FC-2 (Kuiper 2008)", "FC-2 (Renne 1998)"}) {
    o = fit({"--monitors", named});
    EXPECT_EQ(o.code, elctl::kOk) << o.err;
    EXPECT_FALSE(contains(o.out, "which the store does not have")) << o.out;
  }
  // Saved with the default, the level names a set the store has.
  ASSERT_EQ(fit({"--save"}).code, elctl::kOk);
  o = fit({});
  EXPECT_FALSE(contains(o.out, "which the store does not have")) << o.out;
}

TEST_F(FluxCmd, AValueFlagDoesNotTakeTheNextFlag) {
  Outcome o = fit({"--csv", "--save"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--csv needs a value")) << o.err;
  EXPECT_FALSE(fs::exists("--save"));
  o = run_raw({"flux", "fit", "NM-300", "A", "--db", "--save"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--db needs a value")) << o.err;
}

TEST_F(FluxCmd, AnUnwritableCsvSavesNothing) {
  const auto before = seq();
  const Outcome o = fit({"--csv", (path("no-such-dir") / "x.csv").string(), "--save", "--user", "jsmith"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "could not write")) << o.err;
  EXPECT_EQ(o.out, "");
  EXPECT_EQ(seq(), before);
}

TEST_F(FluxCmd, OmitAndExcludeChangeTheFit) {
  const Outcome o = fit({"--omit", "66001-02", "--exclude-position", "3"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  const auto one = table_row(o.out, "Monitors", 1);
  ASSERT_EQ(one.size(), 15u) << o.out;
  EXPECT_EQ(one[3], "2");
  EXPECT_EQ(one.back(), "yes");
  const auto three = table_row(o.out, "Monitors", 3);
  ASSERT_EQ(three.size(), 15u) << o.out;
  EXPECT_EQ(three[3], "3");
  EXPECT_EQ(three.back(), "no");
  EXPECT_TRUE(contains(o.out, "warning: hole 3 left out of the fit")) << o.out;
}

TEST_F(FluxCmd, OmissionsSurviveASaveUntilReset) {
  ASSERT_EQ(fit({"--omit", "66001-02", "--save"}).code, elctl::kOk);
  EXPECT_EQ(table_row(fit({}).out, "Monitors", 1)[3], "2");
  EXPECT_EQ(table_row(fit({"--include", "66001-02"}).out, "Monitors", 1)[3], "3");
  EXPECT_EQ(table_row(fit({"--reset-omits"}).out, "Monitors", 1)[3], "3");
}

// The user's exclusion is carried by a save (R15).
TEST_F(FluxCmd, AnExclusionSurvivesASaveUntilReset) {
  ASSERT_EQ(fit({"--exclude-position", "3", "--save"}).code, elctl::kOk);
  const Outcome again = fit({});
  EXPECT_EQ(table_row(again.out, "Monitors", 3).back(), "no");
  EXPECT_EQ(table_row(again.out, "Monitors", 4).back(), "yes");
  EXPECT_TRUE(contains(again.out, "(4 dof)")) << again.out;
  const Outcome reset = fit({"--reset-omits"});
  EXPECT_EQ(table_row(reset.out, "Monitors", 3).back(), "yes");
  EXPECT_TRUE(contains(reset.out, "(5 dof)")) << reset.out;
}

TEST_F(FluxCmd, AWholeIrradiationContinuesPastAFailingLevel) {
  ASSERT_TRUE(pt::seed_level_without_monitors(*store_, seeded_, "B"));
  const Outcome o = run_raw({"flux", "fit", "NM-300", "--db", db_});
  EXPECT_EQ(o.code, elctl::kFailed) << o.err;
  EXPECT_TRUE(contains(o.out, "NM-300 A")) << o.out;
  EXPECT_TRUE(contains(o.out, "Monitors")) << o.out;
  EXPECT_TRUE(contains(o.out, "66101")) << o.out;
  EXPECT_TRUE(contains(o.err, "NM-300 B")) << o.err;
  EXPECT_TRUE(contains(o.err, "no monitor positions")) << o.err;
}

TEST_F(FluxCmd, PerLevelFlagsNeedALevel) {
  for (const auto& [flag, value] : std::vector<std::pair<std::string, std::string>>{
           {"--omit", "x"}, {"--include", "x"}, {"--exclude-position", "1"}, {"--no-save-position", "1"}}) {
    const Outcome o = run_raw({"flux", "fit", "NM-300", "--db", db_, flag, value});
    EXPECT_EQ(o.code, elctl::kUsage) << flag;
    EXPECT_TRUE(contains(o.err, flag + " needs a level")) << o.err;
  }
}

TEST_F(FluxCmd, WhatIsNotThereIsNamed) {
  Outcome o = fit({"--omit", "99999-01"});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.err, "99999-01")) << o.err;
  o = fit({"--exclude-position", "99"});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.err, "hole 99")) << o.err;
  o = fit({"--no-save-position", "99", "--save"});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.err, "99")) << o.err;
  EXPECT_TRUE(contains(o.err, "holes: 1, 2, 3")) << o.err;
  o = fit({"--monitors", "nope"});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.err, "nope")) << o.err;
  EXPECT_TRUE(contains(o.err, "FC-2 (Kuiper 2008)")) << o.err;
  EXPECT_TRUE(contains(o.err, "Renne")) << o.err;
  o = run_raw({"flux", "fit", "NM-999", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "NM-999")) << o.err;
  o = fit({}, "sqlite:" + path("nothing.db").string());
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_FALSE(fs::exists(path("nothing.db")));
}

TEST_F(FluxCmd, ANoSavePositionIsNotSaved) {
  const Outcome o = fit({"--no-save-position", "12", "--save"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "saved 11 positions (0 unchanged)")) << o.out;
}

TEST_F(FluxCmd, BadFlagsAreUsageErrors) {
  const std::vector<std::vector<std::string>> bad = {
      {"--model", "rbf"},         {"--degree", "9"},           {"--degree", "x"},          {"--neighbors", "0"},
      {"--mean", "median"},       {"--mean-error", "x"},       {"--interpolation", "x"},   {"--axis", "z"},
      {"--weighted", "--unweighted"}, {"--wat"},               {"--model"},                {"--exclude-position", "99999999999"},                {"--fit-error", "sd", "--model", "plane"}};
  for (const auto& extra : bad) {
    const Outcome o = fit(extra);
    EXPECT_EQ(o.code, elctl::kUsage) << extra[0] << "\n" << o.err;
    EXPECT_EQ(o.out, "");
  }
  EXPECT_TRUE(contains(fit({"--fit-error", "sd", "--model", "plane"}).err, "sd is not an error kind of a fitted surface"));
  Outcome o = run_raw({"flux", "fit", "NM-300", "A"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--db")) << o.err;
  o = run_raw({"flux", "fit", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "irradiation")) << o.err;
  o = run_raw({"flux", "fit", "NM-300", "A", "B", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  o = run_raw({"flux", "bogus"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "usage: elctl flux fit")) << o.err;
}

TEST_F(FluxCmd, CsvIsRfc4180AndEveryRowTheHeaderWidth) {
  const std::string tricky = "FC-2, \"new\"";
  const std::string db = make_store("tricky.db", tricky);
  const std::string csv = path("flux.csv").string();
  const Outcome o = fit({"--sample", tricky, "--csv", csv}, db);
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  std::ifstream in(csv, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  const auto rows = parse_csv(text.str());
  ASSERT_EQ(rows.size(), 13u) << text.str();
  for (const auto& row : rows) EXPECT_EQ(row.size(), 19u) << row[0];
  EXPECT_EQ(rows[0][0], "kind");
  EXPECT_EQ(rows[0][18], "notes");
  EXPECT_EQ(rows[1][0], "monitor");
  EXPECT_EQ(rows[1][1], "NM-300");
  EXPECT_EQ(rows[1][2], "A");
  EXPECT_EQ(rows[1][3], "1");
  EXPECT_EQ(rows[1][4], "66001");
  EXPECT_EQ(rows[1][5], tricky);
  EXPECT_EQ(rows[1][8], "3");
  EXPECT_EQ(rows[1][17], "yes");
  EXPECT_EQ(rows[9][0], "unknown");
  EXPECT_EQ(rows[9][4], "66101");
  EXPECT_EQ(rows[9][8], "");
  EXPECT_NE(rows[9][14], "");
  // J is written to 17 digits: it reads back as the golden J of the hole to the fit's accuracy.
  EXPECT_NEAR(std::stod(rows[1][14]), pt::seed_j(1, 1), 1e-3 * pt::seed_j(1, 1));
}

TEST_F(FluxCmd, AWholeIrradiationCsvHoldsEveryLevelOnce) {
  ASSERT_TRUE(pt::seed_level_without_monitors(*store_, seeded_, "B"));
  const std::string csv = path("all.csv").string();
  const Outcome o = run_raw({"flux", "fit", "NM-300", "--db", db_, "--csv", csv});
  EXPECT_EQ(o.code, elctl::kFailed);
  std::ifstream in(csv, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  EXPECT_EQ(parse_csv(text.str()).size(), 13u);  // the header once, then level A
}

// `text` with every run of spaces made one.
std::string collapse(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == ' ' && !out.empty() && out.back() == ' ') continue;
    out += c;
  }
  return out;
}

// The lines of `out` (without the line break).
std::vector<std::string> lines_of(const std::string& out) {
  std::istringstream in(out);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

// The lines of `out` holding `needle`.
std::vector<std::string> lines_with(const std::string& out, const std::string& needle) {
  std::vector<std::string> found;
  for (auto& line : lines_of(out))
    if (contains(line, needle)) found.push_back(std::move(line));
  return found;
}

TEST_F(FluxCmd, ShowListsTheSavedJ) {
  // Before any save every value column is "-".
  Outcome o = run_raw({"flux", "show", "NM-300", "A", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(collapse(o.out), "hole identifier sample J +/- % model saved by saved (UTC)")) << o.out;
  auto lines = lines_of(o.out);
  ASSERT_GE(lines.size(), 13u) << o.out;
  const auto before = split_ws(lines[1 + 8]);  // hole 9, an unknown
  ASSERT_EQ(before.size(), 9u) << lines[9];
  EXPECT_EQ(before[0], "9");
  for (std::size_t i = 3; i < before.size(); ++i) EXPECT_EQ(before[i], "-") << i;

  ASSERT_EQ(fit({"--model", "plane", "--save", "--user", "jsmith"}).code, elctl::kOk);
  o = run_raw({"flux", "show", "NM-300", "A", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  lines = lines_of(o.out);
  ASSERT_EQ(lines.size(), 13u) << o.out;  // the head and the twelve positions
  for (int hole = 1; hole <= 12; ++hole) {
    const auto cells = split_ws(lines[static_cast<std::size_t>(hole)]);
    ASSERT_GE(cells.size(), 9u) << lines[static_cast<std::size_t>(hole)];
    EXPECT_EQ(cells[0], std::to_string(hole));
    EXPECT_NE(cells[3], "-");
    EXPECT_NE(cells[4], "-");
    EXPECT_NE(cells[5], "-");
    EXPECT_EQ(cells[6], "Plane");
    EXPECT_EQ(cells[7], "jsmith");
  }
  EXPECT_TRUE(contains(lines[1], "66001")) << lines[1];
}

TEST_F(FluxCmd, HistoryIsNewestFirstByChangeset) {
  ASSERT_EQ(fit({"--model", "plane", "--save", "--user", "jsmith"}).code, elctl::kOk);
  // The second save leaves hole 9 out, so the two changesets touch different holes.
  ASSERT_EQ(fit({"--model", "nearest", "--neighbors", "3", "--no-save-position", "9", "--save", "--user", "jsmith"}).code,
            elctl::kOk);
  const auto before = seq();
  Outcome o = run_raw({"flux", "history", "NM-300", "A", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  const auto lines = lines_of(o.out);
  ASSERT_EQ(lines.size(), 3u) << o.out;  // the head and two changesets
  EXPECT_TRUE(contains(lines[0], "saved (UTC)")) << o.out;
  EXPECT_TRUE(contains(lines[0], "positions")) << o.out;
  for (std::size_t i = 1; i < 3; ++i) {
    EXPECT_TRUE(contains(lines[i], "jsmith")) << lines[i];
    EXPECT_TRUE(contains(lines[i], "fit flux for NM-300A")) << lines[i];
  }
  // Newest first: the save without hole 9 comes before the save of all twelve.
  EXPECT_TRUE(contains(lines[1], "1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12")) << lines[1];
  EXPECT_FALSE(contains(lines[1], "9,")) << lines[1];
  EXPECT_TRUE(contains(lines[2], "1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12")) << lines[2];

  // One position: a line per revision, newest first, with its J.
  o = run_raw({"flux", "history", "NM-300", "A", "10", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  const auto hole = lines_of(o.out);
  ASSERT_EQ(hole.size(), 3u) << o.out;
  EXPECT_TRUE(contains(hole[0], "J")) << o.out;
  for (std::size_t i = 1; i < 3; ++i) {
    EXPECT_TRUE(contains(hole[i], "fit flux for NM-300A")) << hole[i];
    EXPECT_TRUE(contains(hole[i], "e-")) << hole[i];  // a J in %.4e
  }
  EXPECT_TRUE(contains(hole[1], "Nearest Neighbors")) << hole[1];
  EXPECT_TRUE(contains(hole[2], "Plane")) << hole[2];

  // Hole 9 was saved once.
  o = run_raw({"flux", "history", "NM-300", "A", "9", "--db", db_});
  EXPECT_EQ(lines_of(o.out).size(), 2u) << o.out;
  o = run_raw({"flux", "show", "NM-300", "A", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  o = run_raw({"flux", "monitors", "list", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  o = run_raw({"flux", "monitors", "show", "FC-2 (Kuiper 2008)", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_EQ(seq(), before) << "the read-only subcommands wrote";

  o = run_raw({"flux", "history", "NM-300", "A", "99", "--db", db_});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.err, "99")) << o.err;
}

TEST_F(FluxCmd, MonitorsListsTheDefaults) {
  for (const auto& args : {std::vector<std::string>{"flux", "monitors", "--db", db_},
                           std::vector<std::string>{"flux", "monitors", "list", "--db", db_}}) {
    const Outcome o = run_raw(args);
    EXPECT_EQ(o.code, elctl::kOk) << o.err;
    EXPECT_TRUE(contains(collapse(o.out), "name sample material age (Ma) +/- lambda_k")) << o.out;
    const auto kuiper = lines_with(o.out, "FC-2 (Kuiper 2008)");
    ASSERT_EQ(kuiper.size(), 1u) << o.out;
    EXPECT_EQ(kuiper[0][0], '*');
    EXPECT_TRUE(contains(kuiper[0], "28.201")) << kuiper[0];
    const auto renne = lines_with(o.out, "FC-2 (Renne 1998)");
    ASSERT_EQ(renne.size(), 1u) << o.out;
    EXPECT_NE(renne[0][0], '*');
  }
  const Outcome shown = run_raw({"flux", "monitors", "show", "FC-2 (Renne 1998)", "--db", db_});
  EXPECT_EQ(shown.code, elctl::kOk) << shown.err;
  EXPECT_TRUE(contains(shown.out, "\"name\": \"FC-2 (Renne 1998)\"")) << shown.out;
  EXPECT_TRUE(contains(shown.out, "\"age_ma\"")) << shown.out;
  EXPECT_FALSE(contains(shown.out, "Kuiper")) << shown.out;
  const Outcome none = run_raw({"flux", "monitors", "show", "nope", "--db", db_});
  EXPECT_EQ(none.code, elctl::kFailed);
  EXPECT_TRUE(contains(none.err, "Renne")) << none.err;
}

TEST_F(FluxCmd, MonitorsSetAndDefault) {
  const Outcome shown = run_raw({"flux", "monitors", "show", "FC-2 (Renne 1998)", "--db", db_});
  ASSERT_EQ(shown.code, elctl::kOk);
  // The file holds the two stock sets and a third.
  const Outcome kuiper = run_raw({"flux", "monitors", "show", "FC-2 (Kuiper 2008)", "--db", db_});
  ASSERT_EQ(kuiper.code, elctl::kOk);
  const std::string text = R"j({"default": "FC-2 (Kuiper 2008)", "monitors": [)j" + kuiper.out + ", " + shown.out +
                           R"j(, {"name": "FC-2 (mine)", "sample": "FC-2", "material": "sanidine", "age_ma": 28.3, )j" +
                           R"("age_err_ma": 0.1, "lambda_ec": [5.757e-11, 1.6e-13], )" +
                           R"("lambda_b": [4.955e-10, 1.3e-12]}]})";
  const std::string file = path("sets.json").string();
  {
    std::ofstream out(file);
    out << text;
  }
  Outcome o = run_raw({"flux", "monitors", "set", file, "--db", db_, "--user", "jsmith"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "saved 3 monitor sets")) << o.out;
  o = run_raw({"flux", "monitors", "--db", db_});
  EXPECT_EQ(lines_of(o.out).size(), 4u) << o.out;  // the head and three sets
  EXPECT_TRUE(contains(o.out, "FC-2 (mine)")) << o.out;

  o = run_raw({"flux", "monitors", "default", "FC-2 (mine)", "--db", db_, "--user", "jsmith"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  o = run_raw({"flux", "monitors", "list", "--db", db_});
  EXPECT_EQ(lines_with(o.out, "FC-2 (mine)")[0][0], '*') << o.out;
  EXPECT_NE(lines_with(o.out, "FC-2 (Kuiper 2008)")[0][0], '*') << o.out;
  // The default is what a fit names.
  o = fit({});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "monitors FC-2 (mine): 28.3 +/- 0.1 Ma")) << o.out;
}

TEST_F(FluxCmd, MonitorsSetRejectsABadFile) {
  const auto before = seq();
  const std::string file = path("dup.json").string();
  {
    std::ofstream out(file);
    out << R"({"default": "A", "monitors": [)"
        << R"({"name": "A", "sample": "S", "material": "m", "age_ma": 1, "age_err_ma": 0.1,)"
        << R"( "lambda_ec": [1e-11, 1e-13], "lambda_b": [1e-10, 1e-12]},)"
        << R"({"name": "A", "sample": "S", "material": "m", "age_ma": 1, "age_err_ma": 0.1,)"
        << R"( "lambda_ec": [1e-11, 1e-13], "lambda_b": [1e-10, 1e-12]}]})";
  }
  Outcome o = run_raw({"flux", "monitors", "set", file, "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "flux monitors:")) << o.err;
  EXPECT_TRUE(contains(o.err, "'A' is used twice")) << o.err;
  EXPECT_EQ(seq(), before);
  o = run_raw({"flux", "monitors", "set", path("missing.json").string(), "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "missing.json")) << o.err;
  EXPECT_EQ(seq(), before);
  o = run_raw({"flux", "monitors", "set", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
}

TEST_F(FluxCmd, MonitorsDefaultOfAnUnknownNameListsWhatExists) {
  const auto before = seq();
  const Outcome o = run_raw({"flux", "monitors", "default", "nope", "--db", db_});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.err, "nope")) << o.err;
  EXPECT_TRUE(contains(o.err, "FC-2 (Kuiper 2008)")) << o.err;
  EXPECT_TRUE(contains(o.err, "FC-2 (Renne 1998)")) << o.err;
  EXPECT_EQ(seq(), before);
}

TEST_F(FluxCmd, UnknownSubcommandIsUsage) {
  Outcome o = run_raw({"flux", "nope"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "unknown subcommand 'nope'")) << o.err;
  EXPECT_TRUE(contains(o.err, "usage: elctl flux fit")) << o.err;
  EXPECT_TRUE(contains(o.err, "flux show")) << o.err;
  EXPECT_TRUE(contains(o.err, "flux history")) << o.err;
  EXPECT_TRUE(contains(o.err, "flux monitors")) << o.err;
  EXPECT_EQ(o.out, "");
  o = run_raw({"flux", "show", "NM-300"});
  EXPECT_EQ(o.code, elctl::kUsage);
  o = run_raw({"flux", "monitors", "bogus", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
}

TEST(FluxCmdFormat, CsvFieldQuotesOnlyWhatNeedsIt) {
  EXPECT_EQ(elctl::csv_field("plain"), "plain");
  EXPECT_EQ(elctl::csv_field("a,b"), "\"a,b\"");
  EXPECT_EQ(elctl::csv_field("say \"hi\""), "\"say \"\"hi\"\"\"");
  EXPECT_EQ(elctl::csv_field("two\nlines"), "\"two\nlines\"");
  EXPECT_EQ(elctl::csv_field(""), "");
}

TEST(FluxCmdFormat, ASaveConflictExitsOne) {
  pychron::processing::FluxSaveOutcome outcome;
  outcome.conflict_position = "hole 7";
  ps::Conflict conflict;
  ps::ChangesetInfo by;
  by.created = *ps::UtcTime::parse("2026-10-07T12:30:00Z");
  conflict.actual_by = by;
  outcome.conflict = conflict;
  EXPECT_EQ(elctl::format_flux_save(outcome, "jsmith"),
            "not saved: hole 7 was saved by jsmith at " + by.created.iso() + " since this fit was loaded\n");
  pychron::processing::FluxSaveOutcome saved;
  saved.written = 12;
  EXPECT_EQ(elctl::format_flux_save(saved, ""), "saved 12 positions (0 unchanged)\n");
  saved.written = 0;
  saved.unchanged = 12;
  EXPECT_EQ(elctl::format_flux_save(saved, ""), "nothing to save: 12 positions unchanged\n");
  saved.unchanged = 11;
  saved.skipped = 1;
  EXPECT_EQ(elctl::format_flux_save(saved, ""), "nothing to save: 11 positions unchanged, 1 not saved\n");
}

}  // namespace

#endif
