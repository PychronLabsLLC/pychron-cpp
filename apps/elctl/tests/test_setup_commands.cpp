// elctl init / doctor / --install against the shipped profiles, with a site
// config of the test's own (the fixture sets PYCHRON_SITE_CONFIG).

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "elctl_fixture.hpp"
#include "pychron/setup/site.hpp"

#ifdef PYCHRON_ELCTL_HAS_STORE
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include "pychron/persistence/store.hpp"
#endif

namespace elctl::testing {
namespace {

namespace fs = std::filesystem;

class ElctlSetupTest : public ElctlTest {
 protected:
  pychron::setup::SiteConfig site() const {
    auto s = pychron::setup::load_site(path("site.toml"));
    EXPECT_TRUE(s);
    return s ? *s : pychron::setup::SiteConfig{};
  }
};

TEST_F(ElctlSetupTest, ListsTheProfilesButNotTheFragments) {
  auto o = run_raw({"init", "--list"});
  EXPECT_EQ(o.code, 0) << o.err;
  for (const char* p : {"argus", "helix", "ngx", "data-reduction"}) EXPECT_TRUE(contains(o.out, p)) << o.out;
  EXPECT_FALSE(contains(o.out, "lab-common")) << o.out;
}

TEST_F(ElctlSetupTest, AnInstrumentInstallsChecksAndRunsByName) {
  const auto root = path("argus-lab");
  auto o = run_raw({"init", "argus", "--root", root.string(), "--name", "lab", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "install 'lab' recorded")) << o.out;
  EXPECT_TRUE(contains(o.out, "[OK] spectrometer")) << o.out;
  EXPECT_TRUE(fs::exists(root / "spectrometer.toml"));
  auto s = site();
  ASSERT_NE(s.find("lab"), nullptr);
  EXPECT_EQ(s.default_install, "lab");
  EXPECT_TRUE(s.find("lab")->simulation);

  // A second init changes nothing.
  o = run_raw({"init", "argus", "--root", root.string(), "--name", "lab", "--yes"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "0 file(s) to write")) << o.out;

  o = run_raw({"doctor"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "[OK] lab files")) << o.out;

  // --install supplies the line, the lab and simulation; the queue is found in the install.
  o = run_raw({"--install", "lab", "exp", "validate", "experiment.toml"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "queue first-queue: 5 run(s)")) << o.out;
}

TEST_F(ElctlSetupTest, QuestionsAreAskedWithDefaultsInBrackets) {
  const auto root = path("ngx-lab");
  // Simulation, then the connection, then the detectors: simulation: no;
  // host: Enter (default); port: 1091; user, reference isotope, baseline
  // mass, peak window, reference detector, ion counter, extraction line,
  // where the database is, the database file, the data folder: Enter; then
  // confirm.
  auto o = run_raw({"init", "ngx", "--root", root.string()}, "no\n\n1091\n\n\n\n\n\n\n\n\n\n\ny\n");
  EXPECT_TRUE(contains(o.out, "Where is the data? (local/server) [local]")) << o.out;
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "Address of the NGX controller [192.168.0.20]")) << o.out;
  std::ifstream in(root / "spectrometer.toml");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(contains(text, "kind = \"isotopx_ngx\"")) << text;
  EXPECT_TRUE(contains(text, "port = 1091")) << text;
  EXPECT_FALSE(site().find("ngx")->simulation);
  // On hardware the placeholders are a warning, not a failure.
  o = run_raw({"doctor", "--install", "ngx"});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.err, "[WARN] placeholders")) << o.err;
  EXPECT_EQ(run_raw({"doctor", "--install", "ngx", "--strict"}).code, 1);
}

TEST_F(ElctlSetupTest, ReconfigureRewritesWhatWasNotEdited) {
  const auto root = path("helix-lab");
  ASSERT_EQ(run_raw({"init", "helix", "--root", root.string(), "--yes"}).code, 0);
  std::ofstream(root / "peak_center.toml", std::ios::app) << "# tuned by hand\n";
  auto o = run_raw({"init", "--reconfigure", "--set", "simulation=no", "--set", "qtegra_host=10.1.1.1", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  std::ifstream spec(root / "spectrometer.toml");
  const std::string text((std::istreambuf_iterator<char>(spec)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(contains(text, "host = \"10.1.1.1\"")) << text;
  EXPECT_TRUE(fs::exists(root / "peak_center.toml"));
  EXPECT_FALSE(site().find("helix")->simulation);
}

TEST_F(ElctlSetupTest, DataReductionIsTwoAnswersAndADatabase) {
  const auto root = path("Pychron");
  auto o = run_raw({"init", "data-reduction", "--root", root.string(), "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  const auto config = site();  // find() points into it
  const auto* dr = config.find("data-reduction");
  ASSERT_NE(dr, nullptr);
  EXPECT_EQ(dr->kind, "data_reduction");
#ifdef PYCHRON_ELCTL_HAS_STORE
  EXPECT_TRUE(fs::exists(root / "data" / "pychron.db"));
  EXPECT_TRUE(contains(o.out, "[OK] database")) << o.out;
#endif
}

#ifdef PYCHRON_ELCTL_HAS_STORE
// Install defaults: the instrument's database is made and given the
// references project, its samples and the Triga production.
TEST_F(ElctlSetupTest, AnInstrumentInstallMakesAndSeedsItsDatabase) {
  const auto root = path("argus-seeded");
  auto o = run_raw({"init", "argus", "--root", root.string(), "--name", "lab", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(fs::exists(root / "data" / "pychron.db"));
  EXPECT_TRUE(contains(o.out, "seeded 1 project, 3 materials, 8 samples, 8 identifiers, 1 mass spectrometer, 1 reactor")) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "[OK] database")) << o.out;
  const std::string db = "sqlite:" + (root / "data" / "pychron.db").generic_string();
  auto listed = run_raw({"entry", "samples", "list", "--db", db, "--project", "references"});
  ASSERT_EQ(listed.code, 0) << listed.err;
  for (const char* name : {"blank_unknown", "blank_air", "blank_cocktail", "blank_extractionline", "background", "air",
                           "cocktail", "detector_ic"})
    EXPECT_TRUE(contains(listed.out, std::string(name) + "\treferences")) << name << "\n" << listed.out;

  o = run_raw({"init", "argus", "--root", root.string(), "--name", "lab", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "seed: nothing to add (22 already there)")) << o.out;
}

// Review focus 4: a seed file the lab edited into something unreadable.
TEST_F(ElctlSetupTest, ASeedFileThatDoesNotParseDoesNotStopTheInstall) {
  const auto root = path("helix-bad-seed");
  ASSERT_EQ(run_raw({"init", "helix", "--root", root.string(), "--yes"}).code, 0);
  { std::ofstream(root / "seed.toml") << "project = \n"; }
  auto o = run_raw({"init", "--reconfigure", "--set", "simulation=no", "--yes"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.err, "seed skipped: ")) << o.err;
  EXPECT_TRUE(contains(o.err, "seed.toml")) << o.err;
  EXPECT_TRUE(contains(o.err, "elctl entry seed ")) << o.err;
  EXPECT_FALSE(site().find("helix")->simulation) << "the reconfigure went through";
}

// A seed file that was there before the install and does not parse: the
// database is made all the same (doctor finds it), and the command that is
// printed can be pasted even though the folder's name has a space in it.
TEST_F(ElctlSetupTest, ABadSeedFileStillLeavesADatabaseAndACommandThatCanBePasted) {
  const auto root = path("my argus");
  fs::create_directories(root);
  { std::ofstream(root / "seed.toml") << "project = \n"; }
  auto o = run_raw({"init", "argus", "--root", root.string(), "--name", "lab", "--yes"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(fs::exists(root / "data" / "pychron.db"));
  EXPECT_TRUE(contains(o.out, "[OK] database")) << o.out << o.err;
  EXPECT_TRUE(contains(o.err, "seed skipped: ")) << o.err;
  EXPECT_TRUE(contains(o.err, "elctl entry seed \"" + (root / "seed.toml").string() + "\" --db \"sqlite:")) << o.err;
}

// Review focus 5: a server that is not there.
TEST_F(ElctlSetupTest, ADatabaseServerThatCannotBeReachedSkipsTheSeed) {
  const auto root = path("argus-server");
  auto o = run_raw({"init", "argus", "--root", root.string(), "--yes", "--set", "data_source=server", "--set",
                    "db_host=127.0.0.1", "--set", "db_port=1", "--set", "db_password=s3cret-Zq9"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(fs::exists(root / "seed.toml"));
  EXPECT_TRUE(contains(o.err, "seed skipped: the database could not be opened")) << o.err;
  // The command that has the password: entry seed would need it in its url.
  EXPECT_TRUE(contains(o.err, "elctl init --reconfigure --install argus")) << o.err;
  EXPECT_FALSE(contains(o.err, "entry seed")) << o.err;
  EXPECT_FALSE(contains(o.err, "s3cret-Zq9")) << "the password is not printed";
  EXPECT_FALSE(contains(o.out, "s3cret-Zq9")) << "the password is not printed";
  EXPECT_EQ(site().find("argus")->database, "postgresql://pychron@127.0.0.1:1/pychron");
}

TEST_F(ElctlSetupTest, ADataReductionInstallIsNotSeeded) {
  const auto root = path("dr-plain");
  auto o = run_raw({"init", "data-reduction", "--root", root.string(), "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_FALSE(contains(o.out, "seed")) << o.out;
  EXPECT_FALSE(contains(o.err, "seed")) << o.err;
}
#else
TEST_F(ElctlSetupTest, WithoutTheStoreTheSeedIsSkippedAndSaid) {
  auto o = run_raw({"init", "argus", "--root", path("argus-no-store").string(), "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "skip  seed: built without the DVC store")) << o.out;
}
#endif

TEST_F(ElctlSetupTest, MistakesAreReportedPlainly) {
  EXPECT_EQ(run_raw({"init"}).code, 2);
  auto o = run_raw({"init", "quadrupole", "--yes"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "no profile 'quadrupole'")) << o.err;
  o = run_raw({"init", "argus", "--root", path("x").string(), "--set", "qtegra_port=99999", "--yes"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "not a port")) << o.err;
  o = run_raw({"doctor"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "nothing is installed yet")) << o.err;
  o = run_raw({"--install", "nope", "state"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "no install named 'nope'")) << o.err;
}

TEST_F(ElctlSetupTest, ALabsOwnLineIsImportedAndProbeRunsTheConnectStep) {
  // The fixture's scratch copy of the example line stands in for the lab's files.
  const auto root = path("own-line");
  auto o = run_raw({"init", "argus", "--root", root.string(), "--yes", "--set", "line_source=import", "--set",
                    "line_file=" + path("extraction_line.toml").string(), "--set",
                    "canvas_file=" + path("canvas.toml").string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(std::filesystem::exists(root / "extraction_line.toml"));
  o = run_raw({"doctor", "--install", "argus", "--probe"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "[OK] connect spectrometer: simulated")) << o.out;
  // A line file that is not there stops the install before anything is written.
  o = run_raw({"init", "argus", "--root", path("no-line").string(), "--name", "other", "--yes", "--set",
               "line_source=import", "--set", "line_file=" + path("missing.toml").string(), "--set",
               "canvas_file=" + path("canvas.toml").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "extraction_line.toml: cannot read")) << o.err;
  EXPECT_FALSE(std::filesystem::exists(path("no-line")));
}

// A legacy Pychron setupfiles folder, synthetic, in the formats the legacy
// survey found.
void write_legacy(const fs::path& dir) {
  fs::create_directories(dir / "extractionline");
  fs::create_directories(dir / "devices");
  std::ofstream(dir / "extractionline" / "valves.yaml")
      << "- name: A\n  address: 1\n  interlock: B\n- name: B\n  address: 2\n  query_state: false\n";
  std::ofstream(dir / "devices" / "switch_controller.cfg")
      << "[General]\ntype = NGXGPActuator\n[Communications]\nhost = 10.0.0.5\nport = 1099\n";
}

TEST_F(ElctlSetupTest, ImportLinePrintsOrWritesTheConvertedFiles) {
  const auto legacy = path("setupfiles");
  write_legacy(legacy);
  auto o = run_raw({"import-line", legacy.string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "extractionline/valves.yaml")) << o.out;
  EXPECT_TRUE(contains(o.out, "query_state not carried over (1 valve: B)")) << o.out;
  EXPECT_TRUE(contains(o.out, "kind = \"ngx_valves\"")) << o.out;
  EXPECT_TRUE(contains(o.out, "--- canvas.toml")) << o.out;

  const auto out = path("converted");
  o = run_raw({"import-line", legacy.string(), "--out", out.string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(fs::exists(out / "extraction_line.toml"));
  EXPECT_TRUE(fs::exists(out / "canvas.toml"));
  // Never over what is there, unless asked.
  o = run_raw({"import-line", legacy.string(), "--out", out.string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "exists (--force replaces it)")) << o.err;
  EXPECT_EQ(run_raw({"import-line", legacy.string(), "--out", out.string(), "--force"}).code, 0);

  o = run_raw({"import-line", path("nothing-here").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "is not a folder")) << o.err;
}

TEST_F(ElctlSetupTest, InitConvertsALegacyLineAndSaysWhatWasNotCarriedOver) {
  const auto legacy = path("setupfiles");
  write_legacy(legacy);
  const auto root = path("legacy-lab");
  auto o = run_raw({"init", "argus", "--root", root.string(), "--yes", "--set", "line_source=legacy", "--set",
                    "legacy_folder=" + legacy.string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "Legacy setup conversion:")) << o.out;
  EXPECT_TRUE(contains(o.out, "query_state not carried over")) << o.out;
  std::ifstream in(root / "extraction_line.toml");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(contains(text, "host = \"10.0.0.5\"")) << text;
  EXPECT_TRUE(fs::exists(root / "canvas.toml"));
  // A folder with no legacy line stops the install before anything is written.
  o = run_raw({"init", "argus", "--root", path("no-legacy").string(), "--name", "other", "--yes", "--set",
               "line_source=legacy", "--set", "legacy_folder=" + path("empty").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_FALSE(fs::exists(path("no-legacy")));
}

// elctl data-source (data source editing spec, 3.4).

std::string bytes_of(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST_F(ElctlSetupTest, DataSourceShowsTheDatabaseAndTheDataFolder) {
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("dr").string(), "--yes"}).code, 0);
  auto o = run_raw({"data-source"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "database sqlite:")) << o.out;
  EXPECT_TRUE(contains(o.out, "data folder data")) << o.out;
  EXPECT_FALSE(contains(o.out, "password")) << "a local database has no password: " << o.out;
}

#ifdef PYCHRON_ELCTL_HAS_STORE
// A store one migration behind this build, as ASchemaThatIsBehindSaysWhatToRun
// (test_exp_commands.cpp) makes one.
void make_store_one_migration_behind(const fs::path& file) {
  {
    auto store = pychron::persistence::open_store(pychron::persistence::StoreConfig{"sqlite:" + file.generic_string(), true});
    ASSERT_TRUE(store);
  }
  {
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("data-source-test"));
    d.setDatabaseName(QString::fromStdString(file.string()));
    ASSERT_TRUE(d.open());
    QSqlQuery q(d);
    ASSERT_TRUE(q.exec(QStringLiteral("DELETE FROM schema_version WHERE version = (SELECT max(version) FROM schema_version)")));
    d.close();
  }
  QSqlDatabase::removeDatabase(QStringLiteral("data-source-test"));
}

TEST_F(ElctlSetupTest, SetLocalMakesAndMigratesTheFileAndSeedsAnInstrument) {
  const auto root = path("argus-switch");
  ASSERT_EQ(run_raw({"init", "argus", "--root", root.string(), "--name", "lab", "--yes"}).code, 0);
  auto o = run_raw({"data-source", "set", "--local", "new.db", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(fs::exists(root / "new.db"));
  const std::string url = "sqlite:" + (root / "new.db").generic_string();
  EXPECT_EQ(run_raw({"db", "status", "--db", url}).code, 0);
  EXPECT_EQ(site().find("lab")->database, url);
  EXPECT_TRUE(contains(o.out, "written")) << o.out;
  EXPECT_TRUE(contains(o.out, "seeded ")) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "[OK] database")) << o.out << o.err;
}

TEST_F(ElctlSetupTest, SetLocalRefusesAnOutdatedFile) {
  const auto root = path("dr-outdated");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  const auto old = path("old.db");
  make_store_one_migration_behind(old);
  const std::string url = "sqlite:" + old.generic_string();
  ASSERT_FALSE(pychron::persistence::open_store(pychron::persistence::StoreConfig{url, false}))
      << "the file is meant to be behind";
  const std::string before = bytes_of(path("site.toml"));

  auto o = run_raw({"data-source", "set", "--local", old.string(), "--yes"});
  EXPECT_EQ(o.code, 1) << o.out << o.err;
  EXPECT_TRUE(contains(o.err, "elctl db migrate --db")) << o.err;
  EXPECT_EQ(bytes_of(path("site.toml")), before);
  EXPECT_FALSE(pychron::persistence::open_store(pychron::persistence::StoreConfig{url, false}))
      << "an existing file is never migrated from here";
}
#else
TEST_F(ElctlSetupTest, WithoutTheStoreTheDataSourceIsNotChecked) {
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("dr").string(), "--yes"}).code, 0);
  auto o = run_raw({"data-source", "set", "--local", "new.db", "--yes"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "skip  check: built without the DVC store")) << o.out;
}
#endif

TEST_F(ElctlSetupTest, SetDataAloneKeepsTheDatabase) {
  const auto root = path("dr-data");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  const std::string database = site().find("data-reduction")->database;
  auto o = run_raw({"data-source", "set", "--data", "runs", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  const auto config = site();
  EXPECT_EQ(config.find("data-reduction")->database, database);
  EXPECT_EQ(config.find("data-reduction")->data, "runs");
  EXPECT_TRUE(fs::is_directory(root / "runs"));
}

TEST_F(ElctlSetupTest, SetToTheCurrentSourceWritesNothing) {
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("dr-same").string(), "--yes"}).code, 0);
  const std::string before = bytes_of(path("site.toml"));
  auto o = run_raw({"data-source", "set", "--local", "--yes"});
  EXPECT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "nothing to change")) << o.out;
  EXPECT_EQ(bytes_of(path("site.toml")), before);
}

TEST_F(ElctlSetupTest, SetAsksBeforeWriting) {
  const auto root = path("dr-ask");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  const std::string before = bytes_of(path("site.toml"));
  auto o = run_raw({"data-source", "set", "--data", "runs"}, "n\n");
  EXPECT_EQ(o.code, 1) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "Write them? [Y/n]")) << o.out;
  EXPECT_EQ(bytes_of(path("site.toml")), before);
  EXPECT_FALSE(fs::exists(root / "runs"));
}

// --server: the password from standard input, kept in the credentials file
// and nowhere else; an unreachable server refused unless --no-check.

#ifdef PYCHRON_ELCTL_HAS_STORE
TEST_F(ElctlSetupTest, SetServerUnreachableIsRefused) {
  const auto root = path("dr-unreachable");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  const std::string before = bytes_of(path("site.toml"));
  auto o = run_raw({"data-source", "set", "--server", "127.0.0.1:1/pychron", "--user", "u", "--password-stdin", "--yes"},
                   "pw\n");
  EXPECT_EQ(o.code, 1) << o.out << o.err;
  EXPECT_TRUE(contains(o.err, "postgresql://u@127.0.0.1:1/pychron")) << o.err;
  EXPECT_FALSE(contains(o.out + o.err, "pw@")) << "the password is never said: " << o.out << o.err;
  EXPECT_EQ(bytes_of(path("site.toml")), before);
  EXPECT_FALSE(fs::exists(root / ".pychron" / "credentials.toml"));
}
#endif

TEST_F(ElctlSetupTest, SetServerWithNoCheckIsWritten) {
  const auto root = path("dr-nocheck");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  auto o = run_raw({"data-source", "set", "--server", "127.0.0.1:1/pychron", "--user", "u", "--password-stdin",
                    "--no-check", "--yes"},
                   "pw\r\n");
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "skip  check: --no-check")) << o.out;
  EXPECT_EQ(site().find("data-reduction")->database, "postgresql://u@127.0.0.1:1/pychron");
  const std::string credentials = bytes_of(root / ".pychron" / "credentials.toml");
  EXPECT_TRUE(contains(credentials, "password = \"pw\"")) << credentials;
  EXPECT_FALSE(contains(bytes_of(path("site.toml")), "pw\""));
  EXPECT_FALSE(contains(bytes_of(path("site.toml")), ":pw"));
  EXPECT_FALSE(contains(bytes_of(root / ".pychron" / "install.toml"), "\"pw\""));
}

TEST_F(ElctlSetupTest, SetServerWithoutPasswordStdinKeepsThePassword) {
  const auto root = path("dr-keep");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  ASSERT_EQ(run_raw({"data-source", "set", "--server", "127.0.0.1:1/pychron", "--user", "u", "--password-stdin",
                     "--no-check", "--yes"},
                    "pw\n")
                .code,
            0);
  const fs::path file = root / ".pychron" / "credentials.toml";
  const std::string before = bytes_of(file);
  // Another database on the same server; standard input is no terminal, so
  // nothing is asked and the password is kept.
  auto o = run_raw({"data-source", "set", "--server", "127.0.0.1:1/other", "--user", "u", "--no-check", "--yes"});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_EQ(bytes_of(file), before);
  EXPECT_EQ(site().find("data-reduction")->database, "postgresql://u@127.0.0.1:1/other");
  o = run_raw({"data-source"});
  EXPECT_TRUE(contains(o.out, "password kept")) << o.out;
}

TEST_F(ElctlSetupTest, SetServerUsageMistakes) {
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("dr-server-usage").string(), "--yes"}).code, 0);
  for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
           {"--server", "h", "--user", "u"},              // no /DB
           {"--server", "h/", "--user", "u"},             // empty DB
           {"--server", "h:port/db", "--user", "u"},      // port not a number
           {"--server", "h/db"},                          // no --user
           {"--user", "u", "--data", "runs"},             // --user without --server
           {"--password-stdin", "--data", "runs"},        // a password without --server
           {"--server", "h/db", "--user", "u", "--password-stdin"},  // standard input ended
       }) {
    std::vector<std::string> argv{"data-source", "set"};
    argv.insert(argv.end(), args.begin(), args.end());
    argv.emplace_back("--yes");
    const auto o = run_raw(argv);
    EXPECT_EQ(o.code, 2) << args.front() << " " << o.out << o.err;
  }
}

// Against the lab-like server of the developer's machine, when there is one:
// the base database of PYCHRON_TEST_PG_URL, brought up to date first (its
// public schema; the persistence tests use schemas of their own).
#ifdef PYCHRON_ELCTL_HAS_STORE
struct PgParts {
  std::string host, port = "5432", name, user, password;
};

std::optional<PgParts> parse_pg(std::string url) {
  for (const std::string_view prefix : {"postgresql://", "postgres://"}) {
    if (url.starts_with(prefix)) {
      url.erase(0, prefix.size());
      PgParts p;
      if (const auto q = url.find('?'); q != std::string::npos) url.erase(q);
      const auto at = url.rfind('@');
      if (at == std::string::npos) return std::nullopt;
      const std::string userinfo = url.substr(0, at);
      p.user = userinfo.substr(0, userinfo.find(':'));
      if (const auto colon = userinfo.find(':'); colon != std::string::npos) p.password = userinfo.substr(colon + 1);
      const std::string rest = url.substr(at + 1);
      const auto slash = rest.find('/');
      if (slash == std::string::npos) return std::nullopt;
      p.name = rest.substr(slash + 1);
      const std::string hostport = rest.substr(0, slash);
      const auto colon = hostport.find(':');
      p.host = hostport.substr(0, colon);
      if (colon != std::string::npos) p.port = hostport.substr(colon + 1);
      // A percent-encoded user or password is left to a plainer URL.
      if (p.user.empty() || p.name.empty() || p.host.empty() || contains(userinfo, "%")) return std::nullopt;
      return p;
    }
  }
  return std::nullopt;
}

TEST_F(ElctlSetupTest, SetServerAgainstPostgres) {
  const char* raw = std::getenv("PYCHRON_TEST_PG_URL");
  if (raw == nullptr || *raw == '\0') GTEST_SKIP() << "PYCHRON_TEST_PG_URL not set";
  const auto pg = parse_pg(raw);
  if (!pg) GTEST_SKIP() << "PYCHRON_TEST_PG_URL is not postgresql://user[:password]@host[:port]/db";
  {
    auto store = pychron::persistence::open_store(pychron::persistence::StoreConfig{raw, true});
    ASSERT_TRUE(store) << store.error().what;
  }
  const auto root = path("dr-pg");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--yes"}).code, 0);
  std::vector<std::string> argv{"data-source", "set", "--server", pg->host + ":" + pg->port + "/" + pg->name,
                                "--user", pg->user, "--yes"};
  std::string input;
  if (!pg->password.empty()) {
    argv.emplace_back("--password-stdin");
    input = pg->password + "\n";
  }
  auto o = run_raw(argv, input);
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "[OK] database")) << o.out << o.err;
  EXPECT_EQ(site().find("data-reduction")->database,
            "postgresql://" + pg->user + "@" + pg->host + ":" + pg->port + "/" + pg->name);
}
#endif

TEST_F(ElctlSetupTest, SetUsageMistakes) {
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("dr-usage").string(), "--yes"}).code, 0);
  EXPECT_EQ(run_raw({"data-source", "set", "--local", "--server", "h/db", "--yes"}).code, 2);
  EXPECT_EQ(run_raw({"data-source", "set", "--frobnicate", "--yes"}).code, 2);
  EXPECT_EQ(run_raw({"data-source", "set", "--yes"}).code, 2);
  EXPECT_EQ(run_raw({"data-source", "set", "--data"}).code, 2);
  EXPECT_EQ(run_raw({"data-source", "show-me"}).code, 2);
}

}  // namespace
}  // namespace elctl::testing
