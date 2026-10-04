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
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "git_fixture.hpp"
#include "import_impl.hpp"
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

  static std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  }

  // The settings files in the cache, by name.
  std::vector<std::string> settings_files() const {
    std::vector<std::string> out;
    if (fs::exists(cache_))
      for (const auto& entry : fs::directory_iterator(cache_))
        if (entry.path().extension() == ".toml") out.push_back(entry.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
  }

  // Rows in a table, looked at behind the store.
  long long count(const char* table) const {
    auto db = P::detail::Db::open(P::StoreConfig{db_, false});
    if (!db) return -1;
    auto row = (*db)->select_one(QStringLiteral("SELECT count(*) AS n FROM %1").arg(QString::fromUtf8(table)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  GitFixture repo_;
  LegacyRepoBuilder legacy_{repo_};
  fs::path work_, cache_;
  std::string db_;
};

// What the process had for SIGINT before a test of interrupts.
int g_interrupts_passed_on = 0;
extern "C" void count_interrupt(int) { ++g_interrupts_passed_on; }

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
  const std::string status = import({"status"}).out;
  const auto files = settings_files();
  ASSERT_EQ(files.size(), 1u);
  const std::string settings = read_file(cache_ / files[0]);
  const Outcome o = import({"add", "--kind", "project_repo", "--source", repo_.path().string(), "--tz", "Europe/Paris",
                            "--catalog-from-repos"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "already registered")) << o.err;
  EXPECT_TRUE(contains(o.err, "--tz America/Denver")) << o.err;

  // Nor can the catalog come from another place than it did.
  const Outcome catalog = import({"add", "--kind", "project_repo", "--source", repo_.path().string(), "--tz", kZone});
  EXPECT_EQ(catalog.code, elctl::kUsage);
  EXPECT_TRUE(contains(catalog.err, "already registered")) << catalog.err;
  EXPECT_TRUE(contains(catalog.err, "--catalog-from-repos")) << catalog.err;

  // Neither refusal changed what is stored or what the cache holds.
  EXPECT_EQ(import({"status"}).out, status);
  ASSERT_EQ(settings_files(), files);
  EXPECT_EQ(read_file(cache_ / files[0]), settings);
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
  EXPECT_EQ(import({"status"}).out, "");
  EXPECT_EQ(count("import_source"), 0);
  EXPECT_EQ(settings_files(), std::vector<std::string>{});
}

// A url that carries a user or a password is refused before anything is
// fetched or stored, and the secret is not repeated.
TEST_F(ImportCmd, AddRefusesAUrlWithCredentials) {
  // A password on any scheme; any user on http and https (there it is a
  // token as often as a name); a secret in the query string.
  for (const char* url :
       {"https://alice:s3cret@example.org/lab/IR1010.git", "https://token@example.org/lab/IR1010",
        "http://token@example.org/lab/IR1010", "HTTPS://token@example.org/lab/IR1010",
        "ssh://alice:s3cret@example.org/lab/IR1010.git", "git://alice:s3cret@example.org/lab/IR1010",
        "https://example.org/lab/IR1010?private_token=s3cret", "https://example.org/lab/IR1010?access_TOKEN=s3cret",
        "https://example.org/lab/IR1010?a=1&Password=s3cret", "ssh://example.org/lab/IR1010?secret=s3cret",
        "https://example.org/lab/IR1010?api_key=s3cret"}) {
    const Outcome o = add_project(url);
    EXPECT_EQ(o.code, elctl::kUsage) << url;
    EXPECT_EQ(o.out, "");
    ASSERT_EQ(lines(o.err).size(), 1u) << o.err;
    EXPECT_TRUE(contains(o.err, "credential helper")) << o.err;
    for (const char* secret : {"alice", "s3cret", "example.org", "IR1010", "private_token", "api_key"})
      EXPECT_FALSE(contains(o.err, secret)) << o.err;
  }
  EXPECT_EQ(import({"status"}).out, "");
  EXPECT_EQ(settings_files(), std::vector<std::string>{});
  EXPECT_FALSE(fs::exists(cache_ / "mirrors"));
}

// The rule itself, without a network: a user without a password is how ssh
// names the account, and is not a secret.
TEST(ImportUrl, CarriesCredentials) {
  using elctl::import_detail::url_carries_credentials;
  for (const char* url :
       {"https://alice:s3cret@example.org/lab/IR1010.git", "https://token@example.org/lab/IR1010",
        "http://token@example.org/x", "HtTpS://token@example.org/x", "ssh://alice:s3cret@example.org/x",
        "git://alice:s3cret@example.org/x", "ftp://a:b@example.org/x", "https://example.org/x?private_token=abc",
        "https://example.org/x?access_TOKEN=abc", "https://example.org/x?a=1&Password=abc",
        "ssh://example.org/x?secret=abc", "https://example.org/x?api_key=abc", "https://example.org/x?KEY=abc",
        "https://alice:@example.org/x", "git@example.org:lab/x?token=abc"})
    EXPECT_TRUE(url_carries_credentials(url)) << url;
  for (const char* url :
       {"https://example.org/lab/IR1010.git", "ssh://git@example.org/lab/IR1010.git", "git@example.org:lab/IR1010.git",
        "git://example.org/lab/IR1010", "ssh://git@example.org:2222/lab/IR1010", "https://example.org/lab/x?ref=main",
        "https://example.org/lab/monkey?page=2", "https://example.org/a@b/c", "https://example.org/x#user:pw@frag",
        "/home/me/repos/IR1010", "C:/repos/token/IR1010", "file:///home/me/key=/IR1010", "ssh://git@[::1]:22/x"})
    EXPECT_FALSE(url_carries_credentials(url)) << url;
}

// The settings file is UTF-8 whatever the cache directory is called.
TEST_F(ImportCmd, CacheDirectoryNeedNotBeAscii) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  cache_ = work_ / fs::path(std::u8string(u8"cach\u00e9 Jos\u00e9"));
  const Outcome added = add_project(repo_.url());  // mirrored: the path kept is inside the cache
  ASSERT_EQ(added.code, elctl::kOk) << added.err;
  const auto files = settings_files();
  ASSERT_EQ(files.size(), 1u);
  const std::string settings = read_file(cache_ / files[0]);
  const std::u8string name = u8"cach\u00e9 Jos\u00e9";
  EXPECT_TRUE(contains(settings, std::string(name.begin(), name.end()))) << settings;

  EXPECT_EQ(import({"run"}).code, elctl::kOk);
  EXPECT_TRUE(contains(import({"status"}).out, " repo finished 4/4 "));
  EXPECT_EQ(import({"verify"}).code, elctl::kOk);
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
  if (fs::exists(cache_)) {
    for (const auto& entry : fs::directory_iterator(cache_)) EXPECT_NE(entry.path().extension(), ".toml") << entry.path();
  }
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

  // A dry run fetches nothing: it does not see the next commit, and without
  // the mirror it says so instead of making one.
  legacy_.refit(kRunE, "Ar40", 13.5, kRescaled);
  const Outcome dry = import({"run", "--dry-run"});
  EXPECT_EQ(dry.code, elctl::kOk) << dry.err;
  EXPECT_TRUE(contains(dry.out, "would write 0 rows")) << dry.out;
  fs::remove_all(cache_ / "mirrors");
  const Outcome gone = import({"run", "--dry-run"});
  EXPECT_EQ(gone.code, elctl::kUsage);
  EXPECT_TRUE(contains(gone.err, "a dry run fetches nothing")) << gone.err;
  EXPECT_FALSE(fs::exists(cache_ / "mirrors"));
  EXPECT_EQ(import({"run"}).code, elctl::kOk);
  EXPECT_TRUE(contains(import({"status"}).out, "finished 6/6 " + repo_.head()));
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

// A second Ctrl-C does not wait for the batch: it goes to whatever handled
// the signal before the run (by default, the end of the process).
TEST_F(ImportCmd, ASecondInterruptIsPassedOn) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);

  g_interrupts_passed_on = 0;
  const auto before = std::signal(SIGINT, count_interrupt);
  int batches = 0;
  elctl::set_import_batch_hook([&] {
    if (++batches != 1) return;
    std::raise(SIGINT);
    EXPECT_EQ(g_interrupts_passed_on, 0) << "the first interrupt is the run's";
    std::raise(SIGINT);
    EXPECT_EQ(g_interrupts_passed_on, 1);
  });
  const Outcome o = import({"run", "--batch", "1"});
  elctl::set_import_batch_hook({});
  EXPECT_EQ(std::signal(SIGINT, before), count_interrupt);

  EXPECT_EQ(g_interrupts_passed_on, 1);
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "paused: repo at 1/4")) << o.out;
}

// A paused run exits 0 whatever the sources before the pause left behind.
TEST_F(ImportCmd, APausedRunExitsZeroAfterASourceWithBlockingConflicts) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  repo_.write(LegacyRepoBuilder::path(kRunE, FileKind::Intercepts), "{ this is not json");
  repo_.commit("<ISOEVO> broken", kReviewed);
  const fs::path first = named_copy(repo_, work_ / "sources" / "a_first");  // 5 commits: one batch of 6

  GitFixture other;
  other.init();
  LegacyRepoBuilder later(other);
  later.collect("66052-02", kF.str(), kCollected);
  for (int i = 0; i < 4; ++i) later.refit("66052-02", "Ar40", 12.0 + i, kReviewed);
  const fs::path second = named_copy(other, work_ / "sources" / "b_second");  // 8 commits: two batches

  ASSERT_EQ(add_project(first.string()).code, elctl::kOk);
  ASSERT_EQ(add_project(second.string()).code, elctl::kOk);
  const Outcome o = import({"run", "--batch", "6", "--limit", "2"});
  EXPECT_TRUE(contains(o.out, "a_first: finished 5/5")) << o.out;
  EXPECT_TRUE(contains(o.out, "conflicts pending: 1 blocking")) << o.out;
  EXPECT_TRUE(contains(o.out, "paused: b_second at ")) << o.out;
  EXPECT_EQ(o.code, elctl::kOk);

  // Finished, the same state is exit 1.
  const Outcome done = import({"run"});
  EXPECT_FALSE(contains(done.out, "paused")) << done.out;
  EXPECT_EQ(done.code, elctl::kFailed);
}

TEST_F(ImportCmd, DryRunWritesNothing) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  const std::string before = import({"status"}).out;
  EXPECT_TRUE(contains(before, " registered 0/0 "));
  // Nothing at all: no client, no change log entry, no user.
  const char* const tables[] = {"client", "change_log", "app_user", "analysis", "changeset", "import_conflict"};
  std::vector<long long> rows;
  for (const char* table : tables) rows.push_back(count(table));

  const Outcome dry = import({"run", "--dry-run"});
  EXPECT_EQ(dry.code, elctl::kOk) << dry.err;
  EXPECT_TRUE(contains(dry.out, "repo: dry run, ")) << dry.out;
  EXPECT_TRUE(contains(dry.out, "analyses 1")) << dry.out;
  EXPECT_FALSE(contains(dry.out, "would write 0 rows")) << dry.out;
  EXPECT_EQ(import({"status"}).out, before);
  for (std::size_t i = 0; i < rows.size(); ++i) EXPECT_EQ(count(tables[i]), rows[i]) << tables[i];
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

// A source whose settings file is not in the cache: listed, not guessed at.
TEST_F(ImportCmd, ASourceWithoutItsSettingsFileIsNotRun) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const Outcome added = add_project(repo_.path().string());
  ASSERT_EQ(added.code, elctl::kOk);
  const std::string uuid = lines(added.out).at(0);
  fs::remove(cache_ / (uuid + ".toml"));

  const Outcome status = import({"status"});
  EXPECT_EQ(status.code, elctl::kOk);
  EXPECT_TRUE(contains(status.out, uuid + " project_repo repo registered 0/0")) << status.out;
  for (const auto& args : {std::vector<std::string>{"run"}, {"run", "--dry-run"}, {"verify"}}) {
    const Outcome o = import(args);
    EXPECT_EQ(o.code, elctl::kUsage) << args[0];
    EXPECT_TRUE(contains(o.err, "elctl import: no settings for repo in ")) << o.err;
    EXPECT_TRUE(contains(o.err, uuid + ".toml")) << o.err;
    EXPECT_TRUE(contains(o.err, "run elctl import add for it again")) << o.err;
  }
  EXPECT_TRUE(contains(import({"status"}).out, " registered 0/0")) << "nothing was run";

  // Adding it again restores the file; the source is the same one.
  const Outcome again = add_project(repo_.path().string());
  EXPECT_EQ(again.out, added.out);
  EXPECT_EQ(import({"run"}).code, elctl::kOk);
}

// A dump registered after a project repository was imported: said at once.
TEST_F(ImportCmd, AddOfADumpWarnsAboutProjectsAlreadyImported) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const fs::path imported = named_copy(repo_, work_ / "sources" / "imported");
  const fs::path waiting = named_copy(repo_, work_ / "sources" / "waiting");
  ASSERT_EQ(add_project(imported.string()).code, elctl::kOk);
  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  ASSERT_EQ(add_project(waiting.string()).code, elctl::kOk);

  const Outcome catalog = import({"add", "--kind", "legacy_db", "--source", write_catalog().string(), "--tz", kZone});
  EXPECT_EQ(catalog.code, elctl::kOk) << catalog.err;
  const auto warned = grep(catalog.err, "--replay");
  ASSERT_EQ(warned.size(), 1u) << catalog.err;
  EXPECT_EQ(warned[0], "warning: imported was imported without this dump; its tags may change on the next --replay");
}

// A dump directory that is gone costs the tags, with a warning; it does not
// stop commands on project repositories.
TEST_F(ImportCmd, AMissingDumpGivesNoTagsAndAWarning) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  const fs::path dump = write_catalog();
  ASSERT_EQ(import({"add", "--kind", "legacy_db", "--source", dump.string(), "--tz", kZone}).code, elctl::kOk);
  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  fs::remove_all(dump);

  const Outcome added = add_project(named_copy(repo_, work_ / "sources" / "IR1010").string(), {});
  EXPECT_EQ(added.code, elctl::kOk) << added.err;
  const Outcome ran = import({"run", "--source", "IR1010"});
  EXPECT_EQ(ran.code, elctl::kOk) << ran.out << ran.err;
  const auto warned = grep(ran.err, "warning: no tags from the database dump catalog: ");
  ASSERT_EQ(warned.size(), 1u) << ran.err;
  EXPECT_TRUE(contains(warned[0], "is not there any more")) << warned[0];
  EXPECT_EQ(tag_of(kE), "ok");
  const Outcome verified = import({"verify", "--source", "IR1010"});
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out << verified.err;
  EXPECT_EQ(grep(verified.err, "warning: no tags from the database dump").size(), 1u) << verified.err;
}

// status, conflicts, verify and a dry run read a database; they do not make one.
TEST_F(ImportCmd, ReadingCommandsNeedADatabaseThatExists) {
  const fs::path typo = work_ / "stroe.db";
  const std::string url = "sqlite:" + typo.string();
  for (const auto& args : {std::vector<std::string>{"status"}, {"conflicts"}, {"verify"}, {"run", "--dry-run"}}) {
    std::vector<std::string> full{"import"};
    full.insert(full.end(), args.begin(), args.end());
    full.insert(full.end(), {"--db", url, "--cache", cache_.string()});
    const Outcome o = run_raw(full);
    EXPECT_EQ(o.code, elctl::kUsage) << args[0];
    EXPECT_TRUE(contains(o.err, "elctl import: no database at " + typo.string())) << o.err;
    EXPECT_FALSE(fs::exists(typo)) << args[0];
  }
  // A file that is not a pychron store is not turned into one.
  std::ofstream(typo, std::ios::binary).flush();
  const Outcome empty = run_raw({"import", "status", "--db", url, "--cache", cache_.string()});
  EXPECT_EQ(empty.code, elctl::kUsage);
  EXPECT_TRUE(contains(empty.err, "not a pychron store")) << empty.err;
  EXPECT_EQ(fs::file_size(typo), 0u);
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

// More than twenty unaccounted units: twenty are listed, the rest counted;
// --json lists them all.
TEST_F(ImportCmd, VerifyListsTwentyUnaccountedUnitsAndCountsTheRest) {
  legacy_.collect(kRunE, kE.str(), kCollected);
  for (int i = 0; i < 23; ++i) legacy_.refit(kRunE, "Ar40", 12.0 + i, kReviewed);
  ASSERT_EQ(add_project(repo_.path().string()).code, elctl::kOk);
  ASSERT_EQ(import({"run"}).code, elctl::kOk);
  {
    auto db = P::detail::Db::open(P::StoreConfig{db_, false});
    ASSERT_TRUE(db);
    auto gone = (*db)->affecting(
        QStringLiteral("DELETE FROM import_provenance WHERE entity_type = 'revision' AND path = ?"),
        {P::detail::qv(LegacyRepoBuilder::path(kRunE, FileKind::Intercepts))});
    ASSERT_TRUE(gone);
    ASSERT_EQ(*gone, 24);  // the collection's own version and the 23 refits
  }
  const Outcome verified = import({"verify"});
  EXPECT_EQ(verified.code, elctl::kFailed);
  EXPECT_TRUE(contains(verified.out, " 24 unaccounted")) << verified.out;
  EXPECT_EQ(grep(verified.out, "    imported 660/intercepts/").size(), 20u) << verified.out;
  EXPECT_EQ(grep(verified.out, "    and 4 more").size(), 1u) << verified.out;
  const json report = json::parse(import({"verify", "--json"}).out, nullptr, false);
  ASSERT_TRUE(report.is_array());
  EXPECT_EQ(report[0].at("accounting").at("unaccounted").size(), 24u);
}

// Age parity end to end: the legacy age of the fixture analysis 66052-01E,
// 24.03351976363802 +- 0.9085947707302583 in 660/ia/52.ia.json, against the
// age computed from an import of a history built from the fixture files.
//
// What is real and what is reconstructed. The analysis files, the
// interpreted age, the production and the chronology are byte copies of the
// public repositories. Two things are not the files of 2018-06-05, when the
// interpreted age was saved (tests/dvc/fixtures/README.md, section 1.1):
//   - IC factors. The fixture has the file after the bulk edit of 2021,
//     which multiplied the L2(CDD) factor by the `scalar` it records. The
//     earlier file is rebuilt by dividing value and error by that scalar.
//     That this is the state the age was computed from is supported by the
//     result: with it the legacy age and error are reproduced to 5e-9.
//   - The level file NM-293/G.json is the one at the 2025 head: the fixture
//     has no earlier version. It is committed here under a date before the
//     interpreted age, because reference data is taken as of the age's
//     commit time (spec 10.32): a level file dated 2025 would not have been
//     defined yet. That its J of position 16 is the one the age was computed
//     with is supported by the same result.
//
// The history every test here starts from (build()):
//   meta     2018-01-05  level G, productions.json, Triga_PR, chronology,
//                        sensitivity (one commit)
//   project  2018-02-20  the collection of 66052-01E (four commits)
//            2018-06-05  IC factors before the rescale; then the interpreted
//                        age (kSaved)
//            2021-02-08  the bulk IC-factor edit
// Tests of the as-of rule add meta commits before or after 2018-06-05.
// The residual is 4.9e-9 on the age and 4.5e-9 on the error (computed
// 24.033519881699085 +- 0.9085947748647962): inside the default tolerance of
// 1e-6, outside 1e-9. Where the last 5e-9 comes from is not established (the
// division above is not exact; legacy printed 16 digits).
class ImportCmdParity : public ImportCmd {
 protected:
  // The IC factors of the fixture analysis before the bulk edit.
  static std::string ic_factors_before_rescale() {
    json before = json::parse(LegacyRepoBuilder::fixture_text(FileKind::IcFactors));
    auto& cdd = before.at("L2(CDD)");
    const double scalar = cdd.at("scalar").get<double>();
    cdd["value"] = cdd.at("value").get<double>() / scalar;
    cdd["error"] = cdd.at("error").get<double>() / scalar;
    cdd.erase("scalar");
    return before.dump(4);
  }

  // collection, review, then the interpreted age and the bulk edit in the
  // order asked. `meta_history`: commits to the meta repository after its
  // first, made before anything is imported.
  void build(bool age_saved_before_rescale, const std::function<void()>& meta_history = {},
             const char* meta_date = "2018-01-05T09:30:00-07:00") {
    write_meta(meta_, meta_date);
    if (meta_history) meta_history();
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

  Outcome verify(std::vector<std::string> extra = {}) const {
    std::vector<std::string> args{"verify", "--source", "IR1010"};
    args.insert(args.end(), extra.begin(), extra.end());
    return import(std::move(args));
  }

  // One commit to the meta repository.
  void meta_commit(const std::string& path, const std::string& text, const char* date, const char* message) {
    meta_.write(path, text);
    meta_.commit(message, date);
  }
  void meta_remove(const std::string& path, const char* date, const char* message) {
    meta_.remove(path);
    meta_.commit(message, date);
  }

  // The fixture level file with J of position 16 scaled, or, with `scale` 0,
  // without that position.
  static std::string level_with_j16(double scale) {
    const json fixed = json::parse(fixture("meta/NM-293/G.json"));
    json level = fixed;
    level["positions"] = json::array();
    for (json position : fixed.at("positions")) {
      if (position.value("position", 0) == 16) {
        if (scale == 0) continue;
        position["j"] = position.at("j").get<double>() * scale;
      }
      level["positions"].push_back(std::move(position));
    }
    return level.dump(4);
  }

  // The one member of the interpreted age that is in the store is not
  // comparable, for `reason`.
  void expect_not_comparable(const char* reason) const {
    const Outcome verified = verify();
    EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
    EXPECT_TRUE(contains(verified.out, "parity: 0 pass, 0 pass on age only, 0 fail, 13 not comparable"))
        << verified.out;
    EXPECT_TRUE(contains(verified.out, std::string(reason) + " 1")) << verified.out;
    const json report = json::parse(verify({"--json"}).out, nullptr, false);
    ASSERT_TRUE(report.is_array());
    EXPECT_EQ(report[0].at("parity").at("not_comparable").value(reason, 0), 1);
  }
  void expect_reproduced() const {
    const Outcome verified = verify();
    EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
    EXPECT_TRUE(contains(verified.out, "parity: 1 pass, 0 pass on age only, 0 fail, 12 not comparable "))
        << verified.out;
    // The same residual as with nothing after the age: the same revisions were reduced.
    EXPECT_TRUE(contains(verified.out, "    largest passing residual: age 4.9e-09, error 4.")) << verified.out;
  }

  static constexpr const char* kBeforeTheAge = "2018-03-01T10:00:00-07:00";
  static constexpr const char* kAfterTheAge = "2019-03-01T10:00:00-07:00";

  GitFixture meta_;
};

// The interpreted age lists thirteen analyses; the repository built here has one.
TEST_F(ImportCmdParity, TheLegacyAgeIsReproduced) {
  build(true);
  const Outcome verified = verify();
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
  EXPECT_TRUE(contains(verified.out, "parity: 1 pass, 0 pass on age only, 0 fail, 12 not comparable "
                                     "(constants legacy_preferences; "))
      << verified.out;
  EXPECT_TRUE(contains(verified.out, "not comparable: analysis is not in the store 12")) << verified.out;
  EXPECT_EQ(lines(verified.out).back(), "  ok") << verified.out;
  EXPECT_EQ(import({"conflicts", "--kind", "value_mismatch", "--all"}).out, "");
}

// The default tolerance is 1e-6; the largest residual of the comparisons that
// passed is printed, and kept in --json. A tolerance below the residual fails.
TEST_F(ImportCmdParity, TheLargestPassingResidualIsPrinted) {
  build(true);
  const Outcome verified = verify();
  EXPECT_EQ(verified.code, elctl::kOk) << verified.out;
  // 4.9e-09 on the age and 4.5e-09 on the error (the legacy file has 16 digits).
  EXPECT_TRUE(contains(verified.out, "    largest passing residual: age 4.9e-09, error 4.")) << verified.out;

  const json report = json::parse(verify({"--json"}).out, nullptr, false);
  ASSERT_TRUE(report.is_array());
  const json& parity = report[0].at("parity");
  EXPECT_NEAR(parity.at("max_pass_age_difference").get<double>(), 4.9e-9, 1e-10);
  EXPECT_NEAR(parity.at("max_pass_age_err_difference").get<double>(), 4.5e-9, 1e-10);

  // Below the residual the same data fails.
  const Outcome strict = verify({"--tolerance", "1e-9"});
  EXPECT_EQ(strict.code, elctl::kFailed) << strict.out;
  EXPECT_TRUE(contains(strict.out, "parity: 0 pass, 0 pass on age only, 1 fail")) << strict.out;
  EXPECT_FALSE(contains(strict.out, "largest passing residual")) << strict.out;
}

// The constants are part of the answer: with the lab default (atmospheric
// 40Ar/36Ar 298.56, where legacy used 295.5) the same data gives another
// age. The failure is kept as a conflict that says what it was computed
// with, and a later verify that passes supersedes it.
TEST_F(ImportCmdParity, OtherConstantsDoNotReproduceIt) {
  build(true);
  const Outcome other = verify({"--constants", "default"});
  EXPECT_EQ(other.code, elctl::kFailed) << other.out;
  EXPECT_TRUE(contains(other.out, "parity: 0 pass, 0 pass on age only, 1 fail, 12 not comparable (constants default; "))
      << other.out;
  EXPECT_TRUE(contains(other.out, "    fail " + kE.str() + " in ")) << other.out;
  EXPECT_TRUE(contains(other.out, ": legacy 24.0335197636 +- 0.90859477073, computed 23.97")) << other.out;
  EXPECT_TRUE(contains(other.out, " (age_err_wo_j)")) << other.out;
  EXPECT_EQ(lines(other.out).back(), "  not ok: 1 blocking conflict; 1 parity failure") << other.out;

  const json rows = json::parse(import({"conflicts", "--kind", "value_mismatch", "--json"}).out, nullptr, false);
  ASSERT_TRUE(rows.is_array());
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("detail").value("basis", ""), "constants=default");
  EXPECT_EQ(rows[0].at("detail").value("error_compared", ""), "age_err_wo_j");
  EXPECT_DOUBLE_EQ(rows[0].at("detail").at("legacy").value("age", 0.0), 24.03351976363802);

  const json report = json::parse(verify({"--constants", "default", "--json"}).out, nullptr, false);
  ASSERT_TRUE(report.is_array());
  EXPECT_EQ(report[0].at("parity").value("constants", ""), "default");
  ASSERT_EQ(report[0].at("parity").at("failures").size(), 1u);
  EXPECT_EQ(report[0].at("parity").at("failures")[0].value("basis", ""), "constants=default");

  EXPECT_EQ(verify().code, elctl::kOk);
  EXPECT_EQ(import({"conflicts", "--kind", "value_mismatch"}).out, "");
  EXPECT_EQ(import({"verify", "--source", "IR1010", "--constants", "lee2006"}).code, elctl::kUsage);
}

// Which revisions are reduced. In TheLegacyAgeIsReproduced the head of the
// IC factors is the rescaled file, and the age still agrees: the revision of
// the interpreted age's commit was used. Here the same legacy age is saved
// after the bulk edit, so the rescaled revision is the one in force, and the
// age does not agree. (The other way round to legacy's own history: this is
// a test of the selection, not of a legacy result.)
TEST_F(ImportCmdParity, AnAgeSavedAfterAnEditIsReducedFromTheEditedRevision) {
  build(false);
  const Outcome verified = verify();
  EXPECT_EQ(verified.code, elctl::kFailed) << verified.out;
  EXPECT_TRUE(contains(verified.out, "parity: 0 pass, 0 pass on age only, 1 fail, 12 not comparable")) << verified.out;
}

// A first revision of a kind made later in the store has no parent and no
// provenance, like a root made with the analysis; it is told apart by its
// changeset, and is not taken for the state as of the interpreted age.
//
// The analysis here entered the repository without blanks (its age is
// therefore not the legacy one; the test compares the computed age with
// itself). A blanks revision is then put in behind the store's back, as the
// app would make one later, in a changeset of a later run. The age as of the
// interpreted age must not move.
TEST_F(ImportCmdParity, AParentlessRevisionMadeLaterIsNotTheStateAsOfTheAge) {
  write_meta(meta_, "2018-01-05T09:30:00-07:00");
  legacy_.import_without_collection(kRunE, kE.str(), kCollected, false);
  legacy_.add_interpreted_age(kSaved);
  ASSERT_EQ(import({"add", "--kind", "meta_repo", "--source", meta_.path().string(), "--tz", kZone}).code, elctl::kOk);
  ASSERT_EQ(add_project(named_copy(repo_, work_ / "sources" / "IR1010").string()).code, elctl::kOk);
  ASSERT_EQ(import({"run", "--all"}).code, elctl::kOk);

  const auto computed_age = [&]() -> std::optional<double> {
    const json report = json::parse(verify({"--json"}).out, nullptr, false);
    if (!report.is_array() || report.empty()) return std::nullopt;
    const json& parity = report[0].at("parity");
    if (parity.at("failures").size() != 1) return std::nullopt;
    return parity.at("failures")[0].value("computed_age", 0.0);
  };
  const auto before = computed_age();
  ASSERT_TRUE(before) << verify().out;

  // A later batch: a holder no analysis here refers to.
  meta_.write("irradiation_holders/24_hole.txt", fixture("meta/irradiation_holders/24_hole.txt"));
  meta_.commit("<HOLDER> added later", "2019-03-01T10:00:00-07:00");
  // 1: the age that does not agree is a pending conflict by now.
  ASSERT_EQ(import({"run", "--all"}).code, elctl::kFailed);
  {
    auto db = P::detail::Db::open(P::StoreConfig{db_, false});
    ASSERT_TRUE(db);
    auto later = (*db)->select_one(
        QStringLiteral("SELECT uuid, created_utc FROM changeset WHERE message LIKE '<HOLDER> added later%'"));
    ASSERT_TRUE(later && *later);
    const Uuid made = Uuid::v7();
    auto added = (*db)->affecting(
        QStringLiteral("INSERT INTO revision (uuid, changeset_uuid, subject_type, subject_uuid, analysis_uuid, kind, "
                       "parent_uuid, created_utc) VALUES (?, ?, 'analysis', ?, ?, 'blanks', NULL, ?)"),
        {P::detail::qv(made), (*later)->value("uuid"), P::detail::qv(kE), P::detail::qv(kE),
         (*later)->value("created_utc")});
    ASSERT_TRUE(added) << to_string(added.error());
    ASSERT_EQ(*added, 1);
    added = (*db)->affecting(
        QStringLiteral("INSERT INTO blank_value (revision_uuid, isotope, value, error) VALUES (?, 'Ar40', 5.0, 0.1)"),
        {P::detail::qv(made)});
    ASSERT_TRUE(added) << to_string(added.error());
  }
  auto s = store();
  ASSERT_TRUE(s);
  auto blanks = s->history(kE, Kind::Blanks);
  ASSERT_TRUE(blanks);
  ASSERT_EQ(blanks->size(), 2u);
  EXPECT_FALSE(blanks->front().parent);
  EXPECT_FALSE(blanks->back().parent);
  EXPECT_GT(blanks->back().change_seq, blanks->front().change_seq);
  s.reset();

  const auto after = computed_age();
  ASSERT_TRUE(after) << verify().out;
  EXPECT_EQ(*after, *before);
}

// Spec 10.32 (ruling 52): reference data is taken as of the interpreted
// age's commit time. Revisions made after it are not used: the flux of the
// position, the production's ratios and the chronology all change in 2019,
// and the age is reproduced as before.
TEST_F(ImportCmdParity, ReferenceDataRevisedAfterTheAgeIsTakenAsOfTheAge) {
  build(true);
  meta_commit("NM-293/G.json", level_with_j16(1.01), kAfterTheAge, "<FLUX> refit");
  json production = json::parse(fixture("meta/NM-293/productions/Triga_PR.json"));
  production["K4039"] = json::array({0.0189, 0.0002});
  meta_commit("NM-293/productions/Triga_PR.json", production.dump(4), kAfterTheAge, "modified - Triga_PR.json");
  meta_commit("NM-293/chronology.txt",
              fixture("meta/NM-293/chronology.txt") + "1.0,2017-12-22 06:28:00,2017-12-22 14:28:00\n", kAfterTheAge,
              "second day");
  ASSERT_EQ(import({"run", "--all"}).code, elctl::kOk);
  expect_reproduced();

  // The heads are the 2019 values: reduced from them the age would differ.
  auto s = store();
  ASSERT_TRUE(s);
  auto refs = s->resolve_refs(kE, P::RefPolicy{});
  ASSERT_TRUE(refs);
  int revised = 0;
  for (const auto& ref : refs->refs) {
    auto history = s->history(ref.ref_object, Kind::RefValue);
    ASSERT_TRUE(history);
    if (ref.type == P::RefType::FluxPosition || ref.type == P::RefType::Production ||
        ref.type == P::RefType::Chronology) {
      EXPECT_EQ(history->size(), 2u) << ref.key;
      EXPECT_EQ(ref.revision, history->back().uuid) << ref.key;
      ++revised;
    }
  }
  EXPECT_EQ(revised, 3);
}

// The level-to-production link is revisioned too: the production used is the
// one the level named at the age, as that production stood at the age.
TEST_F(ImportCmdParity, LevelProductionChangedAfterTheAgeIsTakenAsOfTheAge) {
  build(true);
  json production = json::parse(fixture("meta/NM-293/productions/Triga_PR.json"));
  production["K4039"] = json::array({0.0189, 0.0002});
  meta_commit("NM-293/productions/Cd_shielded.json", production.dump(4), kAfterTheAge, "added Cd_shielded");
  json map = json::parse(fixture("meta/NM-293/productions.json"));
  map["G"] = "Cd_shielded";
  meta_commit("NM-293/productions.json", map.dump(4), kAfterTheAge, "level G uses Cd_shielded");
  ASSERT_EQ(import({"run", "--all"}).code, elctl::kOk);
  expect_reproduced();
}

// A level that named, at the age, a production that had no value yet: the
// file of that production came later. Not the production the level named
// before, and not the later file.
TEST_F(ImportCmdParity, ProductionNamedAtTheAgeButDefinedLaterIsNotComparable) {
  build(true, [&] {
    json map = json::parse(fixture("meta/NM-293/productions.json"));
    map["G"] = "Cd_shielded";
    meta_commit("NM-293/productions.json", map.dump(4), kBeforeTheAge, "level G uses Cd_shielded");
    meta_commit("NM-293/productions/Cd_shielded.json", fixture("meta/NM-293/productions/Triga_PR.json"),
                kAfterTheAge, "added Cd_shielded");
  });
  expect_not_comparable("reference_not_yet_defined");
}

// Reference data first written after the age was saved was not there to
// compute it with.
TEST_F(ImportCmdParity, ReferenceDataNotYetDefinedAtTheAgeIsNotComparable) {
  build(true, {}, kAfterTheAge);
  expect_not_comparable("reference_not_yet_defined");
}

// Spec 10.21: reference data removed before the age was saved has no value
// as of the age. Not the value it had before, and not the one it got later.
TEST_F(ImportCmdParity, FluxRemovedAsOfTheAgeIsNotComparable) {
  build(true, [&] {
    meta_commit("NM-293/G.json", level_with_j16(0), kBeforeTheAge, "position 16 emptied");
    meta_commit("NM-293/G.json", level_with_j16(1), kAfterTheAge, "position 16 again");
  });
  expect_not_comparable("no_j");
}

TEST_F(ImportCmdParity, ProductionRemovedAsOfTheAgeIsNotComparable) {
  build(true, [&] {
    meta_remove("NM-293/productions/Triga_PR.json", kBeforeTheAge, "production removed");
    meta_commit("NM-293/productions/Triga_PR.json", fixture("meta/NM-293/productions/Triga_PR.json"), kAfterTheAge,
                "production again");
  });
  expect_not_comparable("no_production");
}

TEST_F(ImportCmdParity, ChronologyRemovedAsOfTheAgeIsNotComparable) {
  build(true, [&] {
    meta_remove("NM-293/chronology.txt", kBeforeTheAge, "chronology removed");
    meta_commit("NM-293/chronology.txt", fixture("meta/NM-293/chronology.txt"), kAfterTheAge, "chronology again");
  });
  expect_not_comparable("no_chronology");
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
  for (const char* word : {"--reference-runs", "--replay", "--dry-run", "--tolerance", "--cache", "unlinked",
                           "--constants", "legacy_preferences", "credential helper", "--all"})
    EXPECT_TRUE(contains(o.out, word)) << word;
}

}  // namespace

#endif  // PYCHRON_ELCTL_IMPORT
