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
    if (!store_) store_ = std::move(*store);
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
      {"--weighted", "--unweighted"}, {"--wat"},               {"--model"},                {"--fit-error", "sd", "--model", "plane"}};
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
}

}  // namespace

#endif
