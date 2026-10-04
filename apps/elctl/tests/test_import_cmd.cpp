// elctl import: the commands a person runs to bring legacy data into the
// store, driven through elctl::run against a file-backed SQLite store and
// repositories built with GitFixture.

#include <gtest/gtest.h>

#include "elctl_fixture.hpp"
#include "import.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_IMPORT

TEST(ImportCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"import", "status", "--db", "sqlite::memory:"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
  EXPECT_EQ(o.out, "");
}

#else

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "git_fixture.hpp"
#include "legacy_repo_builder.hpp"
#include "pychron/ingest/ids.hpp"
#include "pychron/persistence/store.hpp"
#include "tiny/db.hpp"

using namespace pychron;
using namespace pychron::dvc;
using namespace pychron::dvc::testing;
namespace fs = std::filesystem;
namespace P = pychron::persistence;
using nlohmann::json;
using P::Kind;
using P::Uuid;

namespace {

const std::string kRunE = "66052-01E";
const Uuid kE = *Uuid::parse(LegacyRepoBuilder::kFixtureUuid);
const Uuid kF = *Uuid::parse("22222222-2222-4222-8222-222222222222");

const char* const kCollected = "2018-02-20T00:27:10-07:00";
const char* const kReviewed = "2018-06-05T14:57:22-06:00";
const char* const kSaved = "2018-06-05T15:17:44-06:00";   // the interpreted age
const char* const kRescaled = "2021-02-08T10:35:53-07:00";  // the bulk IC-factor edit
const char* const kZone = "America/Denver";

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);)
    if (!line.empty()) out.push_back(line);
  return out;
}

// The lines of `text` that contain `needle`.
std::vector<std::string> grep(const std::string& text, const std::string& needle) {
  std::vector<std::string> out;
  for (auto& line : lines(text))
    if (contains(line, needle)) out.push_back(std::move(line));
  return out;
}

class ImportCmd : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!GitFixture::available()) GTEST_SKIP() << "git not on PATH";
    repo_.init();
    work_ = repo_.temp("work");
    fs::create_directories(work_);
    db_ = "sqlite:" + (work_ / "store.db").string();
    cache_ = work_ / "cache";
  }
  void TearDown() override { elctl::set_import_batch_hook({}); }

  // `elctl import <args> --db <store> --cache <dir>`
  Outcome import(std::vector<std::string> args) const {
    args.insert(args.begin(), "import");
    args.insert(args.end(), {"--db", db_, "--cache", cache_.string()});
    return run_raw(std::move(args));
  }

  // Registers `source` as a project repository; the store has no catalog, so
  // identifiers come from the records.
  Outcome add_project(const std::string& source, std::vector<std::string> extra = {"--catalog-from-repos"}) const {
    std::vector<std::string> args{"add", "--kind", "project_repo", "--source", source, "--tz", kZone};
    args.insert(args.end(), extra.begin(), extra.end());
    return import(std::move(args));
  }

  std::unique_ptr<P::IStore> store() const {
    auto opened = P::open_store(P::StoreConfig{db_, true});
    EXPECT_TRUE(opened) << (opened ? "" : to_string(opened.error()));
    return opened ? std::move(*opened) : nullptr;
  }

  // What a catalog dump would have brought for the fixture analysis.
  void seed_catalog() const {
    auto s = store();
    ASSERT_TRUE(s);
    const Uuid client = *s->register_client({"test", "admin", std::nullopt, "test"});
    ASSERT_TRUE(s->add_mass_spectrometer(client, {"felix", std::nullopt, std::nullopt, std::nullopt}));
    P::IdentifierSpec identifier;
    identifier.identifier = "66052";
    ASSERT_TRUE(s->add_identifier(client, identifier));
    ASSERT_TRUE(s->add_extract_device(client, "Fusions Diode"));
  }

  std::string tag_of(Uuid analysis) const {
    auto s = store();
    if (!s) return {};
    auto head = s->head(analysis, Kind::Tags);
    if (!head || !*head) return {};
    auto payload = s->load_payload(**head);
    if (!payload || !*payload) return {};
    const auto* tag = std::get_if<P::TagValue>(&**payload);
    return tag ? tag->name : std::string();
  }

  // A bare copy of `from` under this test's work directory, named `name`: a
  // source whose name and place in the url order the test decides.
  static fs::path named_copy(GitFixture& from, const fs::path& to) {
    const fs::path clone = from.clone_bare("clone-" + to.filename().string());
    fs::create_directories(to.parent_path());
    fs::copy(clone, to, fs::copy_options::recursive);
    return to;
  }

  // A converted dump with the catalog rows of the fixture analysis and its tag.
  fs::path write_catalog(bool with_manifest = true) const {
    const fs::path dir = work_ / "catalog";
    fs::create_directories(dir);
    const auto put = [&](const char* name, const std::string& text) { std::ofstream(dir / name, std::ios::binary) << text; };
    put("MassSpectrometerTbl.jsonl", R"({"name":"Felix","kind":"Helix SFT"})" "\n");
    put("ExtractDeviceTbl.jsonl", R"({"name":"Fusions Diode"})" "\n");
    put("IrradiationTbl.jsonl", R"({"id":1,"name":"NM-293","create_date":"2018-01-15 17:00:00"})" "\n");
    put("LevelTbl.jsonl", R"({"id":1,"name":"G","irradiationID":1,"holder":null,"z":null,"note":null})" "\n");
    put("IrradiationPositionTbl.jsonl",
        R"({"id":1,"identifier":"66052","sampleID":null,"levelID":1,"position":16,"note":null,"weight":null,"j":null,"j_err":null,"packet":null})"
        "\n");
    put("AnalysisTbl.jsonl",
        R"({"id":1,"experiment_type":"Ar/Ar","timestamp":"2018-02-20 00:27:10","uuid":")" + kE.str() +
            R"(","analysis_type":"unknown","aliquot":1,"increment":4,"irradiation_positionID":1,"mass_spectrometer":"Felix","extract_device":"Fusions Diode"})"
            "\n");
    put("AnalysisChangeTbl.jsonl",
        R"({"idanalysischangeTbl":1,"tag":"omit","timestamp":"2018-06-05 21:16:17","user":"jross","analysisID":1})" "\n");
    if (!with_manifest) return dir;
    json manifest = json::parse(fixture("catalog/MANIFEST.json"));
    json tables = json::object(), files = json::object(), columns = json::object();
    for (const char* table : {"MassSpectrometerTbl", "ExtractDeviceTbl", "IrradiationTbl", "LevelTbl",
                              "IrradiationPositionTbl", "AnalysisTbl", "AnalysisChangeTbl"}) {
      tables[table] = 1;
      files[table] = std::string(table) + ".jsonl";
      columns[table] = manifest.at("columns").at(table);
    }
    manifest["tables"] = tables;
    manifest["files"] = files;
    manifest["columns"] = columns;
    manifest["dump_completed"] = false;
    put("MANIFEST.json", manifest.dump(2));
    return dir;
  }

  // The meta repository of the fixture analysis: its level, production,
  // chronology and sensitivity, committed at `date`.
  static void write_meta(GitFixture& meta, const char* date) {
    meta.init();
    for (const char* file : {"NM-293/G.json", "NM-293/productions.json", "NM-293/productions/Triga_PR.json",
                             "NM-293/chronology.txt", "spectrometers/felix.sens.json"})
      meta.write(file, fixture(std::string("meta/") + file));
    meta.commit("irradiation NM-293", date);
  }

  GitFixture repo_;
  LegacyRepoBuilder legacy_{repo_};
  fs::path work_, cache_;
  std::string db_;
};

// ------------------------------------------------------------------- add

TEST_F(ImportCmd, AddRunStatusVerify) {
  legacy_.collect(kRunE, kE.str(), kCollected);

  const Outcome added = add_project(repo_.path().string());
  ASSERT_EQ(added.code, elctl::kOk) << added.err;
  const auto printed = lines(added.out);
  ASSERT_EQ(printed.size(), 1u) << added.out;
  ASSERT_TRUE(Uuid::parse(printed[0])) << added.out;
  EXPECT_TRUE(fs::exists(cache_ / (printed[0] + ".toml")));

  const Outcome ran = import({"run", "--all"});
  EXPECT_EQ(ran.code, elctl::kOk) << ran.out << ran.err;
  EXPECT_TRUE(contains(ran.err, "repo 4/4 commits, 1 analyses, 1 conflicts")) << ran.err;
  EXPECT_TRUE(contains(ran.out, "repo: finished 4/4")) << ran.out;
  EXPECT_TRUE(contains(ran.out, "analyses 1")) << ran.out;
  EXPECT_TRUE(contains(ran.out, "conflicts pending: 0 blocking, 1 warning (identity_clash 1)")) << ran.out;

  const Outcome status = import({"status"});
  EXPECT_EQ(status.code, elctl::kOk);
  const auto rows = lines(status.out);
  ASSERT_EQ(rows.size(), 1u) << status.out;
  EXPECT_TRUE(contains(rows[0], printed[0] + " project_repo repo finished 4/4 " + repo_.head())) << rows[0];

  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out << verified.err;
  EXPECT_TRUE(contains(verified.out, "import: finished 4/4")) << verified.out;
  EXPECT_TRUE(contains(verified.out, "0 unaccounted")) << verified.out;
  EXPECT_TRUE(contains(verified.out, "idempotence: 0 rows to write on resume, 0 on replay")) << verified.out;
  EXPECT_TRUE(contains(verified.out, "conflicts pending: 0 blocking, 1 warning")) << verified.out;
  EXPECT_EQ(lines(verified.out).back(), "  ok") << verified.out;

  const Outcome as_json = import({"verify", "--json"});
  EXPECT_EQ(as_json.code, elctl::kOk);
  const json report = json::parse(as_json.out, nullptr, false);
  ASSERT_TRUE(report.is_array()) << as_json.out;
  ASSERT_EQ(report.size(), 1u);
  EXPECT_EQ(report[0].value("ok", false), true);
  EXPECT_EQ(report[0].value("name", ""), "repo");
  EXPECT_EQ(report[0].at("accounting").at("unaccounted").size(), 0u);
}

TEST_F(ImportCmd, AddIsIdempotent) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const Outcome first = add_project(repo_.path().string());
  const Outcome second = add_project(repo_.path().string());
  ASSERT_EQ(first.code, elctl::kOk) << first.err;
  ASSERT_EQ(second.code, elctl::kOk) << second.err;
  EXPECT_EQ(first.out, second.out);
  EXPECT_EQ(lines(import({"status"}).out).size(), 1u);
}

TEST_F(ImportCmd, AddRefusesOtherSettingsForARegisteredSource) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  const Outcome o = import({"add", "--kind", "project_repo", "--source", repo_.path().string(), "--tz", "Europe/Paris",
                            "--catalog-from-repos"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "already registered")) << o.err;
}

TEST_F(ImportCmd, AddRequiresTz) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const Outcome o = import({"add", "--kind", "project_repo", "--source", repo_.path().string()});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--tz")) << o.err;
  EXPECT_EQ(import({"status"}).out, "");
}

TEST_F(ImportCmd, AddRejectsUnknownZone) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const Outcome o =
      import({"add", "--kind", "project_repo", "--source", repo_.path().string(), "--tz", "Mars/Olympus_Mons"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "Mars/Olympus_Mons")) << o.err;
  EXPECT_EQ(import({"status"}).out, "");
}

TEST_F(ImportCmd, AddRejectsUnknownKind) {
  const Outcome o = import({"add", "--kind", "spreadsheet", "--source", repo_.path().string(), "--tz", kZone});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--kind")) << o.err;
}

// Each refusal: exit 2, one line that names the path, nothing registered.
void expect_refused(const Outcome& o, const std::string& names, const std::string& reason) {
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_EQ(o.out, "");
  ASSERT_EQ(lines(o.err).size(), 1u) << o.err;
  EXPECT_TRUE(o.err.starts_with("elctl import: ")) << o.err;
  EXPECT_TRUE(contains(o.err, names)) << o.err;
  EXPECT_TRUE(contains(o.err, reason)) << o.err;
}

TEST_F(ImportCmd, AddRejectsNonRepo) {
  const fs::path plain = work_ / "plain";
  fs::create_directories(plain);
  expect_refused(add_project(plain.string()), plain.string(), "not a git repository");
  EXPECT_EQ(import({"status"}).out, "");
  // No settings file either.
  if (fs::exists(cache_))
    for (const auto& entry : fs::directory_iterator(cache_)) EXPECT_NE(entry.path().extension(), ".toml") << entry.path();
}

TEST_F(ImportCmd, AddRejectsMissingBranch) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  expect_refused(add_project(repo_.path().string(), {"--branch", "release"}), repo_.path().string(), "'release'");
  EXPECT_EQ(import({"status"}).out, "");
}

TEST_F(ImportCmd, AddRejectsEmptyRepo) {
  expect_refused(add_project(repo_.path().string()), repo_.path().string(), "empty repository");
  EXPECT_EQ(import({"status"}).out, "");
}

TEST_F(ImportCmd, AddRejectsShallowClone) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const fs::path shallow = repo_.clone_bare("shallow", {"--depth", "1"});
  expect_refused(add_project(shallow.string()), shallow.string(), "shallow");
  EXPECT_EQ(import({"status"}).out, "");
}

TEST_F(ImportCmd, AddRejectsCatalogWithoutManifest) {
  const fs::path dir = write_catalog(false);
  expect_refused(import({"add", "--kind", "legacy_db", "--source", dir.string(), "--tz", kZone}), dir.string(),
                 "MANIFEST.json");
  EXPECT_EQ(import({"status"}).out, "");
}

TEST_F(ImportCmd, AddRejectsAnAuthorMapThatCannotBeRead) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const fs::path map = work_ / "authors.toml";
  std::ofstream(map) << "\"ann@example.org\" = 7\n";
  expect_refused(add_project(repo_.path().string(), {"--catalog-from-repos", "--author-map", map.string()}),
                 map.string(), "ann@example.org");
  EXPECT_EQ(import({"status"}).out, "");
}

// A url is mirrored into the cache and read from there.
TEST_F(ImportCmd, AddMirrorsAUrl) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const Outcome added = add_project(repo_.url());
  ASSERT_EQ(added.code, elctl::kOk) << added.err;
  EXPECT_TRUE(fs::is_directory(cache_ / "mirrors"));

  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  EXPECT_TRUE(contains(import({"status"}).out, "finished 4/4"));

  // The source moves on; the next run fetches and imports the new commit.
  legacy_.refit(kRunE, "Ar40", 12.5, kReviewed);
  const Outcome again = import({"run"});
  EXPECT_EQ(again.code, elctl::kOk) << again.err;
  EXPECT_TRUE(contains(import({"status"}).out, "finished 5/5 " + repo_.head())) << import({"status"}).out;
}

// ------------------------------------------------------------------- run

TEST_F(ImportCmd, RunLimitPausesAndResumes) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kReviewed);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);

  const Outcome first = import({"run", "--batch", "2", "--limit", "1"});
  EXPECT_EQ(first.code, elctl::kOk) << first.err;
  EXPECT_TRUE(contains(first.out, "paused: repo at 2/5")) << first.out;
  EXPECT_EQ(grep(first.err, " commits, ").size(), 1u) << first.err;
  EXPECT_TRUE(contains(import({"status"}).out, " paused 2/5")) << import({"status"}).out;

  const Outcome second = import({"run"});
  EXPECT_EQ(second.code, elctl::kOk) << second.err;
  EXPECT_FALSE(contains(second.out, "paused"));
  EXPECT_TRUE(contains(import({"status"}).out, " finished 5/5"));
  EXPECT_EQ(import({"verify"}).code, elctl::kOk);
}

// Ctrl-C: the batch being written is finished, the run stops there.
TEST_F(ImportCmd, RunStopsAfterTheBatchOnInterrupt) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);

  const auto before = std::signal(SIGINT, SIG_IGN);  // what the process had
  int batches = 0;
  elctl::set_import_batch_hook([&] {
    if (++batches == 1) std::raise(SIGINT);
  });
  const Outcome o = import({"run", "--batch", "1"});
  elctl::set_import_batch_hook({});
  EXPECT_EQ(std::signal(SIGINT, before), SIG_IGN) << "the handler of the run was not taken down";

  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_EQ(batches, 1);
  EXPECT_TRUE(contains(o.out, "paused: repo at 1/4")) << o.out;

  EXPECT_EQ(import({"run", "--batch", "1"}).code, elctl::kOk);
  EXPECT_TRUE(contains(import({"status"}).out, " finished 4/4"));
}

TEST_F(ImportCmd, DryRunWritesNothing) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  const std::string before = import({"status"}).out;
  EXPECT_TRUE(contains(before, " registered 0/0 "));

  const Outcome dry = import({"run", "--dry-run"});
  EXPECT_EQ(dry.code, elctl::kOk) << dry.err;
  EXPECT_TRUE(contains(dry.out, "repo: dry run, ")) << dry.out;
  EXPECT_TRUE(contains(dry.out, "analyses 1")) << dry.out;
  EXPECT_FALSE(contains(dry.out, "would write 0 rows")) << dry.out;
  EXPECT_EQ(import({"status"}).out, before);
  auto s = store();
  ASSERT_TRUE(s);
  auto analysis = s->load_analysis(kE);
  ASSERT_TRUE(analysis);
  EXPECT_FALSE(*analysis);
  s.reset();

  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  const Outcome nothing = import({"run", "--dry-run", "--replay"});
  EXPECT_TRUE(contains(nothing.out, "would write 0 rows")) << nothing.out;
}

TEST_F(ImportCmd, RunNamesAnUnknownSource) {
  const Outcome o = import({"run", "--source", "nowhere"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl import: no source 'nowhere'")) << o.err;
}

// An analysis refused for a missing identifier is imported by a replay once
// the catalog has it, and its conflicts stop counting.
TEST_F(ImportCmd, ReplayImportsWhatWasRefused) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kReviewed);
  ASSERT_EQ(add_project(repo_.path().string(), {}).code, elctl::kOk);

  const Outcome refused = import({"run"});
  EXPECT_EQ(refused.code, elctl::kFailed) << refused.out;
  EXPECT_TRUE(contains(refused.out, "unknown_analysis")) << refused.out;
  EXPECT_TRUE(contains(refused.out, "elctl import run --replay --source repo")) << refused.out;
  const std::size_t pending = lines(import({"conflicts", "--kind", "unknown_analysis"}).out).size();
  EXPECT_GT(pending, 0u);
  EXPECT_EQ(import({"verify"}).code, elctl::kFailed);

  seed_catalog();
  // A plain run has nothing left to walk: the refusals do not hold the token.
  EXPECT_EQ(import({"run"}).code, elctl::kFailed);

  const Outcome replayed = import({"run", "--replay", "--source", "repo"});
  EXPECT_EQ(replayed.code, elctl::kOk) << replayed.out << replayed.err;
  EXPECT_FALSE(contains(replayed.out, "--replay")) << replayed.out;
  EXPECT_EQ(import({"conflicts"}).out, "");
  EXPECT_EQ(grep(import({"conflicts", "--all"}).out, "(superseded)").size(), pending);

  auto s = store();
  ASSERT_TRUE(s);
  auto history = s->history(kE, Kind::Intercepts);
  ASSERT_TRUE(history);
  EXPECT_EQ(history->size(), 2u);
  s.reset();
  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
}

TEST_F(ImportCmd, AuthorMapUsed) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.refit(kRunE, "Ar40", 12.5, kReviewed);
  const fs::path map = work_ / "authors.toml";
  std::ofstream(map) << "\"ann@example.org\" = \"Ann Author\"\n";
  ASSERT_EQ(add_project(repo_.path().string(), {"--catalog-from-repos", "--author-map", map.string()}).code,
            elctl::kOk);
  ASSERT_EQ(import({"run"}).code, elctl::kOk);

  auto s = store();
  ASSERT_TRUE(s);
  auto history = s->history(kE, Kind::Intercepts);
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ(history->back().author_name, "Ann Author");
  s.reset();
  // Verify reads the same settings: nothing to write.
  EXPECT_EQ(import({"verify"}).code, elctl::kOk);
}

// run --all: the catalog, the meta repository, repositories of reference
// runs, then the rest by url. The catalog also supplies the tag of an
// analysis that has no tags file.
TEST_F(ImportCmd, RunAllOrderAndTagsFromTheCatalog) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const fs::path unknowns = named_copy(repo_, work_ / "sources" / "a_unknowns");

  GitFixture blanks;
  blanks.init();
  LegacyRepoBuilder(blanks).collect("66052-02", kF.str(), kCollected);
  const fs::path reference = named_copy(blanks, work_ / "sources" / "z_blanks");

  GitFixture meta;
  write_meta(meta, "2018-01-05T09:30:00-07:00");
  const fs::path metadata = named_copy(meta, work_ / "sources" / "MetaData");

  // Registered in the reverse of the order they must run in.
  ASSERT_EQ(add_project(unknowns.string(), {}).code, elctl::kOk);
  ASSERT_EQ(add_project(reference.string(), {"--reference-runs"}).code, elctl::kOk);
  ASSERT_EQ(import({"add", "--kind", "meta_repo", "--source", metadata.string(), "--tz", kZone}).code, elctl::kOk);
  const Outcome catalog = import({"add", "--kind", "legacy_db", "--source", write_catalog().string(), "--tz", kZone});
  ASSERT_EQ(catalog.code, elctl::kOk) << catalog.err;
  EXPECT_TRUE(contains(catalog.err, "no completion marker")) << catalog.err;

  const auto names = [](const std::string& status) {
    std::vector<std::string> out;
    for (const auto& line : lines(status)) {
      std::istringstream words(line);
      std::string uuid, kind, name;
      words >> uuid >> kind >> name;
      out.push_back(name);
    }
    return out;
  };
  const std::vector<std::string> order{"catalog", "MetaData", "z_blanks", "a_unknowns"};
  EXPECT_EQ(names(import({"status"}).out), order);

  const Outcome ran = import({"run", "--all"});
  EXPECT_EQ(ran.code, elctl::kOk) << ran.out << ran.err;
  EXPECT_TRUE(contains(ran.err, "no completion marker")) << ran.err;
  std::vector<std::string> progressed;
  for (const auto& line : grep(ran.err, " conflicts")) {
    const std::string name = line.substr(0, line.find(' '));
    if (progressed.empty() || progressed.back() != name) progressed.push_back(name);
  }
  EXPECT_EQ(progressed, order) << ran.err;
  EXPECT_TRUE(contains(ran.err, "catalog 5/5 rows, ")) << ran.err;

  EXPECT_EQ(tag_of(kE), "omit");  // from AnalysisChangeTbl
  EXPECT_EQ(tag_of(kF), "ok");    // the dump does not know it

  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out << verified.err;
  EXPECT_EQ(grep(verified.out, "  ok").size(), 4u) << verified.out;

  // One source by name; by uuid.
  const Outcome one = import({"verify", "--source", "MetaData"});
  EXPECT_EQ(one.code, elctl::kOk);
  EXPECT_TRUE(one.out.starts_with("MetaData (meta_repo) ")) << one.out;
  EXPECT_EQ(grep(one.out, "  ok").size(), 1u);
}

// ------------------------------------------------------------------- status, conflicts

TEST_F(ImportCmd, ConflictsListsAndVerifyFails) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  repo_.write(path, "{ this is not json");
  repo_.commit("<ISOEVO> broken", kReviewed);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);

  const Outcome ran = import({"run"});
  EXPECT_EQ(ran.code, elctl::kFailed) << ran.out;
  EXPECT_TRUE(contains(ran.out, "conflicts pending: 1 blocking (unparseable 1), 1 warning (identity_clash 1)"))
      << ran.out;

  const Outcome listed = import({"conflicts", "--kind", "unparseable"});
  EXPECT_EQ(listed.code, elctl::kOk);
  ASSERT_EQ(lines(listed.out).size(), 1u) << listed.out;
  EXPECT_TRUE(listed.out.starts_with("unparseable " + path + " ")) << listed.out;
  EXPECT_FALSE(contains(ran.out, "--replay")) << ran.out;
  EXPECT_EQ(lines(import({"conflicts"}).out).size(), 2u);
  EXPECT_EQ(lines(import({"conflicts", "--source", "repo"}).out).size(), 2u);

  const Outcome as_json = import({"conflicts", "--kind", "unparseable", "--json"});
  const json rows = json::parse(as_json.out, nullptr, false);
  ASSERT_TRUE(rows.is_array()) << as_json.out;
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].value("kind", ""), "unparseable");
  EXPECT_EQ(rows[0].value("path", ""), path);
  EXPECT_EQ(rows[0].value("blocking", false), true);
  EXPECT_TRUE(rows[0].at("detail").is_object());

  const Outcome bad = import({"conflicts", "--kind", "typo"});
  EXPECT_EQ(bad.code, elctl::kUsage);

  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kFailed);
  EXPECT_TRUE(contains(verified.out, "conflicts pending: 1 blocking, 1 warning")) << verified.out;
  EXPECT_EQ(lines(verified.out).back(), "  not ok: 1 blocking conflict") << verified.out;
}

// ------------------------------------------------------------------- verify

TEST_F(ImportCmd, VerifyNeedsAFinishedImport) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  const Outcome never = import({"verify"});
  EXPECT_EQ(never.code, elctl::kFailed);
  EXPECT_TRUE(contains(never.out, "import: registered 0/0")) << never.out;
  EXPECT_TRUE(contains(lines(never.out).back(), "not ok: the import has not finished; run it")) << never.out;

  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  ASSERT_EQ(import({"verify"}).code, elctl::kOk);

  // The source moves on after the import.
  legacy_.refit(kRunE, "Ar40", 12.5, kReviewed);
  const Outcome moved = import({"verify"});
  EXPECT_EQ(moved.code, elctl::kFailed);
  EXPECT_TRUE(contains(lines(moved.out).back(), "the source has changed since the last import")) << moved.out;
}

TEST_F(ImportCmd, VerifyListsWhatIsUnaccounted) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const std::string refit = legacy_.refit(kRunE, "Ar40", 12.5, kReviewed);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  ASSERT_EQ(import({"verify"}).code, elctl::kOk);

  {
    auto db = P::detail::Db::open(P::StoreConfig{db_, false});
    ASSERT_TRUE(db);
    auto gone = (*db)->affecting(
        QStringLiteral("DELETE FROM import_provenance WHERE entity_type = 'revision' AND commit_sha = ?"),
        {P::detail::qv(refit)});
    ASSERT_TRUE(gone);
    ASSERT_EQ(*gone, 1);
  }
  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kFailed);
  EXPECT_TRUE(contains(verified.out, "1 unaccounted")) << verified.out;
  const std::string path = LegacyRepoBuilder::path(kRunE, FileKind::Intercepts);
  EXPECT_TRUE(contains(verified.out, "imported " + path + " @" + refit)) << verified.out;
  EXPECT_TRUE(contains(verified.out, "missing revision " + path + " @" + refit)) << verified.out;
  EXPECT_TRUE(contains(lines(verified.out).back(), "not ok: 1 unaccounted")) << verified.out;

  const json report = json::parse(import({"verify", "--json"}).out, nullptr, false);
  ASSERT_TRUE(report.is_array());
  EXPECT_EQ(report[0].value("ok", true), false);
  ASSERT_EQ(report[0].at("accounting").at("unaccounted").size(), 1u);
  EXPECT_EQ(report[0].at("accounting").at("unaccounted")[0].value("path", ""), path);
}

// Age parity end to end. The meta repository gives J, production and
// chronology. The project repository holds the fixture analysis and the
// interpreted age that stores its legacy age, 24.0335 Ma.
//
// The fixtures cannot reproduce that age to the default 1e-9. The IC factors
// kept are the ones a bulk edit rescaled three years after the age was saved,
// and the level file is the one of 2025 (tests/dvc/fixtures/README.md,
// section 1.1); what the repository and the meta repository held in 2018 is
// not among them. From the fixture's IC factors the age is 24.0364 (1.2e-4
// off); with the rescale undone, as these tests reconstruct the earlier
// state, it is 23.9754 (2.4e-3 off). The tests therefore use a tolerance
// that admits the difference where they need a pass, and show which state
// was reduced by the age that comes out.
class ImportCmdParity : public ImportCmd {
 protected:
  static constexpr const char* kTolerance = "5e-3";
  static constexpr double kAgeBeforeRescale = 23.9753501817, kAgeAfterRescale = 24.0363615215;

  // The IC factors of the fixture analysis before the bulk edit.
  static std::string ic_factors_before_rescale() {
    json before = json::parse(LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
    auto& cdd = before.at("L2(CDD)");
    cdd["value"] = cdd.at("value").get<double>() / cdd.at("scalar").get<double>();
    cdd.erase("scalar");
    return before.dump(4);
  }

  // collection, review, then the interpreted age and the bulk edit in the order asked.
  void build(bool age_saved_before_rescale) {
    write_meta(meta_, "2018-01-05T09:30:00-07:00");
    legacy_.collect(kRunE, kE.str(), kCollected);
    legacy_.write(kRunE, FileKind::IcFactors, ic_factors_before_rescale());
    legacy_.commit("<ICFactor> auto update ic_factors, fits=L2(CDD)(Bracketing Interpolate)", kReviewed);
    if (age_saved_before_rescale) legacy_.add_interpreted_age(kSaved);
    legacy_.write(kRunE, FileKind::IcFactors, LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
    legacy_.commit("<ICFactor> bulk edit scaled icfactor by 0.9932318104906938", kRescaled);
    if (!age_saved_before_rescale) legacy_.add_interpreted_age("2021-03-01T10:00:00-07:00");

    ASSERT_EQ(import({"add", "--kind", "meta_repo", "--source", meta_.path().string(), "--tz", kZone}).code,
              elctl::kOk);
    ASSERT_EQ(add_project(named_copy(repo_, work_ / "sources" / "IR1010").string()).code, elctl::kOk);
    const Outcome ran = import({"run", "--all"});
    ASSERT_EQ(ran.code, elctl::kOk) << ran.out << ran.err;
  }

  Outcome verify() const { return import({"verify", "--source", "IR1010", "--tolerance", kTolerance}); }

  // The age verify computed for the fixture analysis, from the failure it
  // reports at the default tolerance.
  double computed_age() const {
    const json report = json::parse(import({"verify", "--source", "IR1010", "--json"}).out, nullptr, false);
    if (!report.is_array() || report.empty()) return 0;
    const json& failures = report[0].at("parity").at("failures");
    if (failures.size() != 1 || failures[0].value("analysis", "") != kE.str()) return 0;
    return failures[0].value("computed_age", 0.0);
  }

  GitFixture meta_;
};

// The interpreted age lists thirteen analyses; the repository built here has
// one. Its age is saved before the bulk edit: the age comes from the IC
// factors of that commit, not from the head.
TEST_F(ImportCmdParity, AgeIsComputedFromTheStateAsOfTheInterpretedAge) {
  build(true);
  // At the default tolerance the difference is a failure, kept as a conflict
  // holding both values.
  const Outcome strict = import({"verify", "--source", "IR1010"});
  EXPECT_EQ(strict.code, elctl::kFailed);
  EXPECT_TRUE(contains(strict.out, "parity: 0 pass, 0 pass on age only, 1 fail, 12 not comparable")) << strict.out;
  EXPECT_TRUE(contains(strict.out, "not comparable: analysis is not in the store 12")) << strict.out;
  EXPECT_TRUE(contains(strict.out, "    fail " + kE.str() + " in ")) << strict.out;
  EXPECT_TRUE(contains(strict.out, ": legacy 24.0335197636 +- 0.90859477073, computed 23.975")) << strict.out;
  EXPECT_EQ(lines(strict.out).back(), "  not ok: 1 blocking conflict; 1 parity failure") << strict.out;
  EXPECT_EQ(lines(import({"conflicts", "--kind", "value_mismatch"}).out).size(), 1u);
  EXPECT_NEAR(computed_age(), kAgeBeforeRescale, 1e-6);

  // A verify that passes supersedes the conflict.
  const Outcome verified = verify();
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
  EXPECT_TRUE(contains(verified.out, "parity: 1 pass, 0 pass on age only, 0 fail, 12 not comparable")) << verified.out;
  EXPECT_EQ(lines(verified.out).back(), "  ok") << verified.out;
  EXPECT_EQ(import({"conflicts", "--kind", "value_mismatch"}).out, "");
}

// Saved after the bulk edit, the same history gives the age of the rescaled
// IC factors.
TEST_F(ImportCmdParity, AnAgeSavedLaterIsComputedFromTheLaterState) {
  build(false);
  EXPECT_NEAR(computed_age(), kAgeAfterRescale, 1e-6);
  const Outcome verified = import({"verify", "--source", "IR1010", "--tolerance", "1e-3"});
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
  EXPECT_TRUE(contains(verified.out, "parity: 1 pass, 0 pass on age only, 0 fail, 12 not comparable")) << verified.out;
}

// Reference data that changed after the interpreted age was saved: the state
// the age was computed from is gone, and the member is not compared (10.32).
TEST_F(ImportCmdParity, ReferenceDataChangedAfterTheAgeIsNotComparable) {
  build(true);
  json level = json::parse(fixture("meta/NM-293/G.json"));
  for (auto& position : level.at("positions"))
    if (position.value("position", 0) == 16) position["j"] = position.at("j").get<double>() * 1.01;
  meta_.write("NM-293/G.json", level.dump(4));
  meta_.commit("<FLUX> refit", "2019-03-01T10:00:00-07:00");
  ASSERT_EQ(import({"run", "--all"}).code, elctl::kOk);

  const Outcome verified = verify();
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
  EXPECT_TRUE(contains(verified.out, "parity: 0 pass, 0 pass on age only, 0 fail, 13 not comparable")) << verified.out;
  EXPECT_TRUE(contains(verified.out, "not comparable: reference_changed_after 1")) << verified.out;

  const json report = json::parse(import({"verify", "--source", "IR1010", "--json"}).out, nullptr, false);
  ASSERT_TRUE(report.is_array());
  EXPECT_EQ(report[0].at("parity").at("not_comparable").value("reference_changed_after", 0), 1);
}

// Without the meta repository there is no J: not comparable, never a number.
TEST_F(ImportCmdParity, NoAgeIsInventedWithoutReferenceData) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  legacy_.add_interpreted_age(kSaved);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
  EXPECT_TRUE(contains(verified.out, "parity: 0 pass, 0 pass on age only, 0 fail, 13 not comparable")) << verified.out;
  EXPECT_TRUE(contains(verified.out, "no_j 1")) << verified.out;
}

// ------------------------------------------------------------------- usage

TEST_F(ImportCmd, UnknownSubcommand) {
  const Outcome o = import({"frobnicate"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "frobnicate")) << o.err;
  EXPECT_TRUE(contains(o.err, "usage: elctl import")) << o.err;
}

TEST_F(ImportCmd, NeedsASubcommandAndADatabase) {
  EXPECT_EQ(run_raw({"import"}).code, elctl::kUsage);
  const Outcome o = run_raw({"import", "status"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "--db")) << o.err;
  const Outcome flag = run_raw({"import", "status", "--db", db_, "--frob"});
  EXPECT_EQ(flag.code, elctl::kUsage);
  EXPECT_TRUE(contains(flag.err, "--frob")) << flag.err;
}

TEST_F(ImportCmd, BadDbUrl) {
  const Outcome o = run_raw({"import", "status", "--db", "oracle://nowhere/db"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(o.err.starts_with("elctl import: ")) << o.err;
  EXPECT_EQ(o.out, "");
}

TEST_F(ImportCmd, HelpDescribesTheCommands) {
  const Outcome top = run_raw({"help"});
  EXPECT_TRUE(contains(top.out, "import add")) << top.out;
  const Outcome o = run_raw({"import", "help"});
  EXPECT_EQ(o.code, elctl::kOk);
  for (const char* word : {"--reference-runs", "--replay", "--dry-run", "--tolerance", "--cache", "unlinked"})
    EXPECT_TRUE(contains(o.out, word)) << word;
}

}  // namespace

#endif  // PYCHRON_ELCTL_IMPORT
