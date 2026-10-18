// Changing an install's data source (data source editing spec, 3.1): the
// current source read from the install record or the site entry, the URL it
// stands for, a planned change, refused before anything is written, and the
// change applied: credentials, install record and site entry, nothing else.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "pychron/setup/data_source.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"
#include "pychron/setup/template.hpp"
#include "temp_path.hpp"

using namespace pychron;
using namespace pychron::setup;
namespace fs = std::filesystem;

namespace {

void write_text(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every file and directory under `dir`, by relative name, with the sha256 of
// a file's bytes: a file written, made or removed shows.
std::map<std::string, std::string> hash_tree(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    const std::string name = fs::relative(entry.path(), dir).generic_string();
    out[name] = entry.is_regular_file() ? sha256_hex(read_text(entry.path())) : std::string("dir");
  }
  return out;
}

// A temp directory holding an install root and a site file beside it.
struct DataSourceFixture : ::testing::Test {
  fs::path dir = pychron::testing::unique_temp_path("pychron-data-source-");
  fs::path root = dir / "lab";
  fs::path site_path = dir / "site" / "site.toml";

  void SetUp() override { fs::create_directories(root); }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  [[nodiscard]] SiteInstall install(std::string database = {}, std::string data = "data") const {
    SiteInstall s;
    s.name = "lab";
    s.kind = "data_reduction";
    s.profile = "data-reduction";
    s.root = root;
    s.database = database.empty() ? "sqlite:" + (root / "data" / "pychron.db").generic_string() : std::move(database);
    s.data = std::move(data);
    return s;
  }

  void write_record(const Answers& answers) const {
    InstallRecord record;
    record.profile = "data-reduction";
    record.versions = {{"store-common", 2}};
    record.answers = answers;
    ASSERT_TRUE(write_install_record(root, record));
  }

  [[nodiscard]] std::string records_note(const std::string& old_data, const std::string& new_data) const {
    return "records in " + (root / old_data).generic_string() + "/records are not moved; " +
           (root / new_data).generic_string() + " starts without them";
  }

  [[nodiscard]] std::string no_record_note() const {
    return "no install record in " + (root / ".pychron").generic_string() +
           ": a later elctl init --reconfigure would not know the new data source";
  }
};

Answers server_answers() {
  return {{"data_source", Value{std::string("server")}},
          {"db_host", Value{std::string("h")}},
          {"db_port", Value{std::int64_t{5433}}},
          {"db_name", Value{std::string("n")}},
          {"db_user", Value{std::string("u")}},
          {"data_folder", Value{std::string("runs")}}};
}

bool contains(const std::vector<std::string>& notes, const std::string& note) {
  return std::find(notes.begin(), notes.end(), note) != notes.end();
}

}  // namespace

TEST_F(DataSourceFixture, CurrentSourceComesFromTheRecord) {
  write_record(server_answers());
  auto current = current_data_source(install());  // the site entry says local
  ASSERT_TRUE(current) << current.error().what;
  EXPECT_EQ(current->database, (std::variant<LocalDatabase, ServerDatabase>{
                                   ServerDatabase{"h", 5433, "n", "u", std::nullopt}}));
  EXPECT_EQ(current->data, "runs");
}

TEST_F(DataSourceFixture, CurrentSourceFallsBackToTheSiteEntry) {
  const fs::path file = root / "x.db";
  auto local = current_data_source(install("sqlite:" + file.generic_string(), "d"));
  ASSERT_TRUE(local) << local.error().what;
  ASSERT_TRUE(std::holds_alternative<LocalDatabase>(local->database));
  EXPECT_EQ(std::get<LocalDatabase>(local->database).file.generic_string(), file.generic_string());
  EXPECT_EQ(local->data, "d");

  auto server = current_data_source(install("postgresql://u@h:5432/n"));
  ASSERT_TRUE(server) << server.error().what;
  EXPECT_EQ(server->database,
            (std::variant<LocalDatabase, ServerDatabase>{ServerDatabase{"h", 5432, "n", "u", std::nullopt}}));

  // A password in the URL is not taken: the kept one is the credentials file's.
  auto with_pw = current_data_source(install("postgresql://u:secret@h:5432/n"));
  ASSERT_TRUE(with_pw) << with_pw.error().what;
  EXPECT_EQ(with_pw->database,
            (std::variant<LocalDatabase, ServerDatabase>{ServerDatabase{"h", 5432, "n", "u", std::nullopt}}));
}

TEST_F(DataSourceFixture, AnEncodedUserInTheSiteEntryIsDecoded) {
  // The installer writes the user percent-encoded (me@lab as me%40lab).
  const SiteInstall before = install("postgresql://me%40lab@h:5432/n", "runs");
  auto current = current_data_source(before);
  ASSERT_TRUE(current) << current.error().what;
  EXPECT_EQ(current->database,
            (std::variant<LocalDatabase, ServerDatabase>{ServerDatabase{"h", 5432, "n", "me@lab", std::nullopt}}));
  EXPECT_EQ(database_url(*current, root), before.database);

  // A plan to the source as it stands is no change.
  auto change = plan_data_source(before, *current);
  ASSERT_TRUE(change) << change.error().what;
  EXPECT_EQ(change->notes, (std::vector<std::string>{"nothing to change"}));
  EXPECT_EQ(change->install.database, before.database);

  // A '%' with no two hex digits after it is kept.
  auto odd = current_data_source(install("postgresql://a%zz%4@h:5432/n"));
  ASSERT_TRUE(odd) << odd.error().what;
  EXPECT_EQ(std::get<ServerDatabase>(odd->database).user, "a%zz%4");
}

TEST_F(DataSourceFixture, AVersionOneRecordIsTheDefaultFileAndFolder) {
  write_record({{"data_source", Value{std::string("local")}}});
  auto current = current_data_source(install());
  ASSERT_TRUE(current) << current.error().what;
  EXPECT_EQ(current->database, (std::variant<LocalDatabase, ServerDatabase>{LocalDatabase{"data/pychron.db"}}));
  EXPECT_EQ(current->data, "data");
}

TEST_F(DataSourceFixture, DatabaseUrlIsTheSiteEntrysUrl) {
  EXPECT_EQ(database_url(DataSource{LocalDatabase{"lab/copy.db"}, "data"}, root),
            "sqlite:" + (root / "lab/copy.db").generic_string());
  const fs::path absolute = dir / "shared" / "pychron.db";
  EXPECT_EQ(database_url(DataSource{LocalDatabase{absolute}, "data"}, root), "sqlite:" + absolute.generic_string());
  EXPECT_EQ(database_url(DataSource{ServerDatabase{"h", 5433, "n", "u", std::nullopt}, "data"}, root),
            "postgresql://u@h:5433/n");
  EXPECT_EQ(database_url(DataSource{ServerDatabase{"h", 5433, "n", "u", "secret"}, "data"}, root),
            "postgresql://u@h:5433/n");
}

TEST_F(DataSourceFixture, TheUrlWithPasswordUsesTheGivenOrTheKeptOne) {
  const DataSource given{ServerDatabase{"h", 5433, "n", "u", "p@ss"}, "data"};
  auto url = database_url_with_password(given, root);
  ASSERT_TRUE(url) << url.error().what;
  EXPECT_EQ(*url, "postgresql://u:p%40ss@h:5433/n");

  const DataSource kept{ServerDatabase{"h", 5433, "n", "u", std::nullopt}, "data"};
  auto none = database_url_with_password(kept, root);
  ASSERT_TRUE(none) << none.error().what;
  EXPECT_EQ(*none, "postgresql://u@h:5433/n");

  write_text(root / ".pychron" / "credentials.toml", "[database]\npassword = \"k\"\n");
  auto from_file = database_url_with_password(kept, root);
  ASSERT_TRUE(from_file) << from_file.error().what;
  EXPECT_EQ(*from_file, "postgresql://u:k@h:5433/n");

  const DataSource local{LocalDatabase{"a.db"}, "data"};
  auto file = database_url_with_password(local, root);
  ASSERT_TRUE(file) << file.error().what;
  EXPECT_EQ(*file, database_url(local, root));
}

TEST_F(DataSourceFixture, DescribeSaysTheUrlAndTheFolder) {
  const DataSource source{LocalDatabase{"a.db"}, "runs"};
  EXPECT_EQ(describe(source, root), "database sqlite:" + (root / "a.db").generic_string() + "\ndata folder runs (" +
                                        (root / "runs").generic_string() + ")");
}

TEST_F(DataSourceFixture, RefusalsWriteNothing) {
  write_record(server_answers());
  fs::create_directories(root / "adir");
  const auto before = hash_tree(dir);
  const ServerDatabase good{"h", 5432, "n", "u", std::nullopt};
  auto server = [&](auto change) {
    ServerDatabase s = good;
    change(s);
    return DataSource{s, "data"};
  };
  const std::vector<std::pair<std::string, DataSource>> refused = {
      {"empty host", server([](ServerDatabase& s) { s.host.clear(); })},
      {"empty name", server([](ServerDatabase& s) { s.name.clear(); })},
      {"empty user", server([](ServerDatabase& s) { s.user.clear(); })},
      {"port 0", server([](ServerDatabase& s) { s.port = 0; })},
      {"port 65536", server([](ServerDatabase& s) { s.port = 65536; })},
      {"empty file", DataSource{LocalDatabase{""}, "data"}},
      {"empty data", DataSource{LocalDatabase{"data/pychron.db"}, ""}},
      {"file is a directory", DataSource{LocalDatabase{"adir"}, "data"}},
  };
  for (const auto& [why, to] : refused) {
    auto plan = plan_data_source(install(), to);
    ASSERT_FALSE(plan) << why;
    EXPECT_EQ(plan.error().kind, ErrorKind::Config) << why;
  }
  SiteInstall gone = install();
  gone.root = dir / "not-there";
  auto plan = plan_data_source(gone, DataSource{LocalDatabase{"data/pychron.db"}, "data"});
  ASSERT_FALSE(plan);
  EXPECT_EQ(plan.error().kind, ErrorKind::Config);
  EXPECT_EQ(hash_tree(dir), before);
}

TEST_F(DataSourceFixture, APlanSetsTheSiteFieldsAndSaysWhatIsNotMoved) {
  write_record({{"data_source", Value{std::string("local")}}});
  write_text(root / "data" / "records" / "a.toml", "x = 1\n");
  const DataSource to{LocalDatabase{"data/pychron.db"}, "other"};
  auto change = plan_data_source(install(), to);
  ASSERT_TRUE(change) << change.error().what;
  EXPECT_EQ(change->install.data, "other");
  EXPECT_EQ(change->install.database, database_url(to, root));
  EXPECT_TRUE(contains(change->notes, records_note("data", "other")));

  fs::remove(root / "data" / "records" / "a.toml");
  auto empty = plan_data_source(install(), to);
  ASSERT_TRUE(empty) << empty.error().what;
  EXPECT_TRUE(empty->notes.empty());
}

TEST_F(DataSourceFixture, AnInstallWithoutARecordIsToldSo) {
  auto change = plan_data_source(install(), DataSource{ServerDatabase{"h", 5432, "n", "u", std::nullopt}, "data"});
  ASSERT_TRUE(change) << change.error().what;
  EXPECT_TRUE(contains(change->notes, no_record_note()));
  EXPECT_EQ(change->install.database, "postgresql://u@h:5432/n");
}

TEST_F(DataSourceFixture, APlanToTheCurrentSourceChangesNothing) {
  write_record(server_answers());
  const SiteInstall before = install("postgresql://u@h:5433/n", "runs");
  auto current = current_data_source(before);
  ASSERT_TRUE(current) << current.error().what;
  auto change = plan_data_source(before, *current);
  ASSERT_TRUE(change) << change.error().what;
  EXPECT_EQ(change->notes, (std::vector<std::string>{"nothing to change"}));
  EXPECT_EQ(change->install.database, before.database);
  EXPECT_EQ(change->install.data, before.data);
  EXPECT_EQ(change->install.root, before.root);
  EXPECT_EQ(change->install.name, before.name);
}

TEST_F(DataSourceFixture, PathsOutsideTheRootAndWithSpacesAreKeptAsGiven) {
  write_record({{"data_source", Value{std::string("local")}}});
  const std::string shared = (dir / "shared disk" / "runs").generic_string();
  const DataSource to{LocalDatabase{"my data/p.db"}, shared};
  auto change = plan_data_source(install(), to);
  ASSERT_TRUE(change) << change.error().what;
  EXPECT_EQ(change->install.data, shared);
  EXPECT_EQ(change->install.database, "sqlite:" + (root / "my data/p.db").generic_string());
}

namespace {

const fs::path kProfiles(PYCHRON_PROFILES_DIR);
const fs::path kExamples(PYCHRON_EXAMPLE_CONFIGS_DIR);

ProfileLibrary library() {
  auto lib = ProfileLibrary::load(kProfiles, kExamples);
  EXPECT_TRUE(lib) << (lib ? "" : lib.error().what);
  return std::move(*lib);
}

const ServerDatabase kServer{"h", 5432, "n", "u", std::nullopt};

ServerDatabase server_with(std::optional<std::string> password) {
  ServerDatabase s = kServer;
  s.password = std::move(password);
  return s;
}

// The fixture with the site file and real data-reduction installs.
struct DataSourceApply : DataSourceFixture {
  fs::path creds = root / ".pychron" / "credentials.toml";

  // Installs the shipped data-reduction profile into `root` and records it
  // in the site file as "lab".
  SiteInstall install_profile(const Answers& given) {
    const auto lib = library();
    auto resolved = lib.resolve("data-reduction");
    EXPECT_TRUE(resolved) << (resolved ? "" : resolved.error().what);
    auto answers = complete_answers(*resolved, given, builtin_answers("lab", root));
    EXPECT_TRUE(answers) << (answers ? "" : answers.error().what);
    auto plan = plan_install(lib, *resolved, *answers, root);
    EXPECT_TRUE(plan) << (plan ? "" : plan.error().what);
    auto report = apply_install(*plan);
    EXPECT_TRUE(report) << (report ? "" : report.error().what);
    SiteInstall entry = site_install(*plan, "lab");
    EXPECT_TRUE(register_install(entry, site_path));
    return entry;
  }

  // What `elctl init --reconfigure` plans: the recorded answers, plus `extra`.
  [[nodiscard]] InstallPlan reconfigure(const Answers& extra = {}) const {
    const auto lib = library();
    auto resolved = lib.resolve("data-reduction");
    EXPECT_TRUE(resolved) << (resolved ? "" : resolved.error().what);
    auto record = read_install_record(root);
    EXPECT_TRUE(record) << (record ? "" : record.error().what);
    Answers given = record->answers;
    for (const auto& [id, v] : extra) given[id] = v;
    const bool keep_secrets = keep_unanswered_secrets(*resolved, given);
    auto answers = complete_answers(*resolved, given, builtin_answers("lab", root));
    EXPECT_TRUE(answers) << (answers ? "" : answers.error().what);
    auto plan = plan_install(lib, *resolved, *answers, root, {.reconfigure = true, .skip_secret_files = keep_secrets});
    EXPECT_TRUE(plan) << (plan ? "" : plan.error().what);
    return std::move(*plan);
  }

  // Plans and applies `to` for the install named "lab" in the site file.
  void apply(const DataSource& to) const {
    auto site = load_site(site_path);
    ASSERT_TRUE(site) << site.error().what;
    const SiteInstall* entry = site->find("lab");
    ASSERT_NE(entry, nullptr);
    auto change = plan_data_source(*entry, to);
    ASSERT_TRUE(change) << change.error().what;
    auto applied = apply_data_source(*change, to, site_path);
    ASSERT_TRUE(applied) << applied.error().what;
  }

  [[nodiscard]] SiteInstall site_entry(const std::string& name = "lab") const {
    auto site = load_site(site_path);
    EXPECT_TRUE(site) << (site ? "" : site.error().what);
    const SiteInstall* entry = site->find(name);
    EXPECT_NE(entry, nullptr);
    return entry ? *entry : SiteInstall{};
  }

  static void expect_no_conflict(const InstallPlan& plan) {
    for (const auto& f : plan.files)
      EXPECT_NE(f.action, PlannedFile::Action::Conflict) << f.to.generic_string();
  }
};

bool same_entry(const SiteInstall& a, const SiteInstall& b) {
  return a.name == b.name && a.kind == b.kind && a.profile == b.profile && a.root == b.root && a.line == b.line &&
         a.canvas == b.canvas && a.spectrometer == b.spectrometer && a.data == b.data && a.database == b.database &&
         a.simulation == b.simulation;
}

}  // namespace

TEST_F(DataSourceApply, ApplyLocalToServerWritesTheThreeFiles) {
  install_profile({});
  SiteInstall other = install("sqlite:/elsewhere/other.db", "other-data");
  other.name = "other";
  other.root = dir / "other";
  other.line = "x.toml";
  ASSERT_TRUE(register_install(other, site_path));
  {
    auto site = load_site(site_path);
    ASSERT_TRUE(site);
    site->default_install = "other";
    ASSERT_TRUE(save_site(*site, site_path));
  }
  const SiteInstall before = site_entry();

  apply(DataSource{server_with("s3cret"), "runs"});

  auto kept = read_kept_password(root);
  ASSERT_TRUE(kept) << kept.error().what;
  EXPECT_EQ(*kept, std::optional<std::string>("s3cret"));
#ifndef _WIN32
  EXPECT_EQ(fs::status(creds).permissions() & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
#endif

  auto record = read_install_record(root);
  ASSERT_TRUE(record) << record.error().what;
  EXPECT_EQ(record->answers.at("data_source"), Value{std::string("server")});
  EXPECT_EQ(record->answers.at("db_host"), Value{std::string("h")});
  EXPECT_EQ(record->answers.at("db_port"), Value{std::int64_t{5432}});
  EXPECT_EQ(record->answers.at("db_name"), Value{std::string("n")});
  EXPECT_EQ(record->answers.at("db_user"), Value{std::string("u")});
  EXPECT_EQ(record->answers.at("data_folder"), Value{std::string("runs")});
  EXPECT_FALSE(record->answers.contains("db_file"));
  EXPECT_FALSE(record->answers.contains("db_password"));
  EXPECT_EQ(record->files.at(".pychron/credentials.toml"), sha256_hex(read_text(creds)));

  auto site = load_site(site_path);
  ASSERT_TRUE(site) << site.error().what;
  EXPECT_EQ(site->default_install, "other");
  const SiteInstall* after = site->find("lab");
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->database, "postgresql://u@h:5432/n");
  EXPECT_EQ(after->data, "runs");
  SiteInstall expected = before;
  expected.database = after->database;
  expected.data = after->data;
  EXPECT_TRUE(same_entry(*after, expected));
  const SiteInstall* other_after = site->find("other");
  ASSERT_NE(other_after, nullptr);
  EXPECT_TRUE(same_entry(*other_after, other));

  EXPECT_EQ(read_text(record_path(root)).find("s3cret"), std::string::npos);
  EXPECT_EQ(read_text(site_path).find("s3cret"), std::string::npos);
}

TEST_F(DataSourceApply, AKeptPasswordLeavesTheCredentialsFileAlone) {
  install_profile({{"data_source", Value{std::string("server")}},
                   {"db_host", Value{std::string("old")}},
                   {"db_password", Value{std::string("kept")}}});
  const std::string bytes = read_text(creds);
  auto before = read_install_record(root);
  ASSERT_TRUE(before) << before.error().what;

  apply(DataSource{server_with(std::nullopt), "data"});

  EXPECT_EQ(read_text(creds), bytes);
  auto after = read_install_record(root);
  ASSERT_TRUE(after) << after.error().what;
  EXPECT_EQ(after->files.at(".pychron/credentials.toml"), before->files.at(".pychron/credentials.toml"));
  EXPECT_EQ(after->answers.at("db_host"), Value{std::string("h")});
}

TEST_F(DataSourceApply, ApplyKeepsAPasswordWithSpecialCharacters) {
  install_profile({});
  const std::string password = R"(p@ss:w/"r\d)";
  apply(DataSource{server_with(password), "data"});

  auto kept = read_kept_password(root);
  ASSERT_TRUE(kept) << kept.error().what;
  EXPECT_EQ(*kept, std::optional<std::string>(password));
  auto url = database_url_with_password(DataSource{server_with(std::nullopt), "data"}, root);
  ASSERT_TRUE(url) << url.error().what;
  EXPECT_NE(url->find("p%40ss%3Aw%2F%22r%5Cd"), std::string::npos) << *url;
}

TEST_F(DataSourceApply, TheCredentialsFileIsWhatTheProfileRenders) {
  install_profile({});
  apply(DataSource{server_with("x"), "data"});
  auto rendered = render(read_text(kProfiles / "store-common" / "credentials.toml"),
                         {{"db_password", Value{std::string("x")}}});
  ASSERT_TRUE(rendered) << rendered.error().what;
  EXPECT_EQ(read_text(creds), *rendered);
}

TEST_F(DataSourceApply, AReconfigureKeepsTheNewSource) {
  install_profile({});
  apply(DataSource{server_with("pw"), "runs"});
  const SiteInstall applied = site_entry();

  const InstallPlan kept = reconfigure();
  EXPECT_EQ(site_install(kept, "lab").database, applied.database);
  EXPECT_EQ(site_install(kept, "lab").data, applied.data);
  expect_no_conflict(kept);

  const InstallPlan again = reconfigure({{"db_password", Value{std::string("pw")}}});
  EXPECT_EQ(site_install(again, "lab").database, applied.database);
  EXPECT_EQ(site_install(again, "lab").data, applied.data);
  expect_no_conflict(again);
  const auto it = std::ranges::find(again.files, fs::path(".pychron/credentials.toml"), &PlannedFile::to);
  ASSERT_NE(it, again.files.end());
  EXPECT_TRUE(it->action == PlannedFile::Action::Update || it->action == PlannedFile::Action::Same)
      << to_string(it->action);
}

TEST_F(DataSourceApply, AReconfigureAfterServerToLocalHasNoConflicts) {
  install_profile({{"data_source", Value{std::string("server")}},
                   {"db_host", Value{std::string("h")}},
                   {"db_password", Value{std::string("pw")}}});
  apply(DataSource{LocalDatabase{"copy.db"}, "data"});
  EXPECT_EQ(site_entry().database, "sqlite:" + (root / "copy.db").generic_string());

  const InstallPlan plan = reconfigure();
  EXPECT_EQ(site_install(plan, "lab").database, site_entry().database);
  expect_no_conflict(plan);
  EXPECT_TRUE(fs::exists(creds));
  auto record = read_install_record(root);
  ASSERT_TRUE(record) << record.error().what;
  EXPECT_EQ(record->answers.at("db_file"), Value{std::string("copy.db")});
  for (const char* id : {"db_host", "db_port", "db_name", "db_user"}) EXPECT_FALSE(record->answers.contains(id)) << id;
}

TEST_F(DataSourceApply, OnlyTheThreeFilesChange) {
  install_profile({});
  const auto root_before = hash_tree(root);
  const auto site_before = hash_tree(site_path.parent_path());

  apply(DataSource{server_with("pw"), "runs"});

  auto root_after = hash_tree(root);
  auto site_after = hash_tree(site_path.parent_path());
  std::vector<std::string> changed;
  for (const auto& [name, hash] : root_after) {
    const auto it = root_before.find(name);
    if (it == root_before.end() || it->second != hash) changed.push_back(name);
  }
  for (const auto& [name, hash] : root_before)
    if (!root_after.contains(name)) changed.push_back("removed " + name);
  EXPECT_EQ(changed, (std::vector<std::string>{".pychron/credentials.toml", ".pychron/install.toml"}));
  EXPECT_EQ(site_after.size(), site_before.size());
  EXPECT_NE(site_after.at("site.toml"), site_before.at("site.toml"));
}

TEST_F(DataSourceApply, WithoutARecordOnlyTheSiteAndCredentialsAreWritten) {
  ASSERT_TRUE(register_install(install(), site_path));
  apply(DataSource{server_with("pw"), "data"});
  EXPECT_FALSE(fs::exists(record_path(root)));
  EXPECT_TRUE(fs::exists(creds));
  EXPECT_EQ(site_entry().database, "postgresql://u@h:5432/n");
}

// --- Checks before writing (spec 3.3), and the seed after ---

namespace {

// Openers that record what they were asked and answer as told.
struct FakeOpeners {
  std::vector<std::pair<std::string, bool>> opened;  // (url, migrate)
  struct Seeded {
    std::string url;
    fs::path seed_file;
    bool migrate = false;
  };
  std::vector<Seeded> seeded;
  Result<std::string> open_reply = std::string("schema version 12");
  Result<std::string> seed_reply = std::string("seeded 3 samples");

  DataSourceOpeners openers() {
    DataSourceOpeners o;
    o.open_database = [this](const std::string& url, bool migrate) {
      opened.emplace_back(url, migrate);
      return open_reply;
    };
    o.seed_database = [this](const std::string& url, const fs::path& seed_file, bool migrate) {
      seeded.push_back({url, seed_file, migrate});
      return seed_reply;
    };
    return o;
  }
};

bool any_contains(const std::vector<std::string>& lines, const std::string& part) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; });
}

DataSource local_source(const fs::path& file, std::string data = "data") {
  DataSource s;
  s.database = LocalDatabase{file};
  s.data = std::move(data);
  return s;
}

DataSource server_source(std::optional<std::string> password) {
  DataSource s;
  s.database = ServerDatabase{"db.lab", 5432, "pychron", "ana", std::move(password)};
  return s;
}

struct DataSourceCheckTest : DataSourceFixture {
  [[nodiscard]] DataSourceChange change_for(const DataSource& to, const std::string& kind = "data_reduction") const {
    SiteInstall s = install();
    s.kind = kind;
    s.database = database_url(to, root);
    s.data = to.data;
    return DataSourceChange{s, {}};
  }
};

}  // namespace

TEST_F(DataSourceCheckTest, ANewLocalFileIsMadeAndMigrated) {
  const DataSource to = local_source("sub/new.db");
  {
    FakeOpeners fake;
    auto checked = check_data_source(change_for(to), to, fake.openers(), false);
    ASSERT_TRUE(checked) << checked.error().what;
    EXPECT_TRUE(fake.opened.empty());
    EXPECT_TRUE(any_contains(checked->said, "will be made"));
    EXPECT_FALSE(checked->made_file);
    EXPECT_FALSE(checked->opened);
    EXPECT_FALSE(fs::exists(root / "sub"));
  }
  FakeOpeners fake;
  auto checked = check_data_source(change_for(to), to, fake.openers(), true);
  ASSERT_TRUE(checked) << checked.error().what;
  ASSERT_EQ(fake.opened.size(), 1U);
  EXPECT_EQ(fake.opened[0].first, "sqlite:" + (root / "sub" / "new.db").generic_string());
  EXPECT_TRUE(fake.opened[0].second);
  EXPECT_TRUE(fs::is_directory(root / "sub"));
  EXPECT_TRUE(checked->made_file);
  EXPECT_TRUE(checked->opened);
}

TEST_F(DataSourceCheckTest, AnExistingLocalFileIsNeverMigrated) {
  write_text(root / "old.db", "not empty");
  const DataSource to = local_source("old.db");
  const std::string url = "sqlite:" + (root / "old.db").generic_string();
  for (const bool create : {true, false}) {
    FakeOpeners fake;
    fake.open_reply = fail(ErrorKind::Config, "schema version 9 is behind 12\nmore");
    auto checked = check_data_source(change_for(to), to, fake.openers(), create);
    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error().kind, ErrorKind::Config);
    std::string expected = url;
    expected += ": schema version 9 is behind 12; run `elctl db migrate --db ";
    expected += url;
    expected += "` first";
    EXPECT_EQ(checked.error().what, expected);
    EXPECT_NE(checked.error().what.find("elctl db migrate --db sqlite:"), std::string::npos);
    ASSERT_EQ(fake.opened.size(), 1U);
    EXPECT_FALSE(fake.opened[0].second);
  }
  FakeOpeners fake;
  auto checked = check_data_source(change_for(to), to, fake.openers(), true);
  ASSERT_TRUE(checked) << checked.error().what;
  EXPECT_FALSE(checked->made_file);
  EXPECT_TRUE(checked->opened);
  ASSERT_EQ(fake.opened.size(), 1U);
  EXPECT_FALSE(fake.opened[0].second);
}

TEST_F(DataSourceCheckTest, AnEmptyFileIsNotAStore) {
  write_text(root / "empty.db", "");
  const DataSource to = local_source("empty.db");
  FakeOpeners fake;
  auto checked = check_data_source(change_for(to), to, fake.openers(), true);
  ASSERT_FALSE(checked);
  EXPECT_EQ(checked.error().kind, ErrorKind::Config);
  EXPECT_EQ(checked.error().what, (root / "empty.db").generic_string() + " is empty: not a pychron store");
  EXPECT_TRUE(fake.opened.empty());
}

TEST_F(DataSourceCheckTest, AServerIsOpenedWithThePasswordGivenOrKept) {
  {
    const DataSource to = server_source(std::string("given"));
    FakeOpeners fake;
    auto checked = check_data_source(change_for(to), to, fake.openers(), true);
    ASSERT_TRUE(checked) << checked.error().what;
    ASSERT_EQ(fake.opened.size(), 1U);
    EXPECT_NE(fake.opened[0].first.find("given"), std::string::npos);
    EXPECT_FALSE(fake.opened[0].second);
    EXPECT_TRUE(checked->opened);
    EXPECT_FALSE(checked->made_file);
    EXPECT_FALSE(any_contains(checked->said, "given"));
  }
  write_text(root / ".pychron" / "credentials.toml", "[database]\npassword = \"kept-pw\"\n");
  const DataSource kept = server_source(std::nullopt);
  {
    FakeOpeners fake;
    auto checked = check_data_source(change_for(kept), kept, fake.openers(), false);
    ASSERT_TRUE(checked) << checked.error().what;
    ASSERT_EQ(fake.opened.size(), 1U);
    EXPECT_EQ(fake.opened[0].first, "postgresql://ana:kept-pw@db.lab:5432/pychron");
    EXPECT_FALSE(fake.opened[0].second);
  }
  FakeOpeners fake;
  fake.open_reply = fail(ErrorKind::Io, "could not connect with ana:kept-pw\nsecond line");
  auto checked = check_data_source(change_for(kept), kept, fake.openers(), true);
  ASSERT_FALSE(checked);
  EXPECT_EQ(checked.error().kind, ErrorKind::Config);
  EXPECT_EQ(checked.error().what.find("kept-pw"), std::string::npos);
  EXPECT_NE(checked.error().what.find("postgresql://ana@db.lab:5432/pychron: could not connect"), std::string::npos);
  EXPECT_NE(checked.error().what.find("check the server, the user and the password"), std::string::npos);
  EXPECT_EQ(checked.error().what.find("second line"), std::string::npos);
  ASSERT_EQ(fake.opened.size(), 1U);
  EXPECT_FALSE(fake.opened[0].second);
}

TEST_F(DataSourceCheckTest, WithoutAnOpenerTheDatabaseIsNotChecked) {
  const DataSource to = local_source("new.db");
  auto checked = check_data_source(change_for(to), to, DataSourceOpeners{}, true);
  ASSERT_TRUE(checked) << checked.error().what;
  EXPECT_TRUE(contains(checked->said, "skip  check: built without the DVC store"));
  EXPECT_FALSE(checked->opened);
  EXPECT_FALSE(checked->made_file);
  EXPECT_FALSE(fs::exists(root / "new.db"));
}

TEST_F(DataSourceCheckTest, TheDataFolderIsMadeOrRefused) {
  const DataSource to = local_source("x.db", "runs/new");
  {
    FakeOpeners fake;
    auto checked = check_data_source(change_for(to), to, fake.openers(), false);
    ASSERT_TRUE(checked) << checked.error().what;
    EXPECT_FALSE(fs::exists(root / "runs"));
    EXPECT_TRUE(contains(checked->said, "data folder " + (root / "runs" / "new").generic_string() + " will be made"));
  }
  {
    FakeOpeners fake;
    auto checked = check_data_source(change_for(to), to, fake.openers(), true);
    ASSERT_TRUE(checked) << checked.error().what;
    EXPECT_TRUE(fs::is_directory(root / "runs" / "new"));
    EXPECT_TRUE(fs::is_empty(root / "runs" / "new"));  // the probe is gone
  }
#ifdef _WIN32
  GTEST_SKIP() << "POSIX permissions";
#else
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  const fs::path locked = root / "locked";
  fs::create_directories(locked);
  fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
  const DataSource to_locked = local_source("x.db", "locked");
  for (const bool create : {true, false}) {
    FakeOpeners fake;
    auto checked = check_data_source(change_for(to_locked), to_locked, fake.openers(), create);
    EXPECT_FALSE(checked);
    if (!checked) {
      EXPECT_EQ(checked.error().what, "data folder " + locked.generic_string() + " is not writable");
    }
    EXPECT_TRUE(fake.opened.empty());
  }
  fs::permissions(locked, fs::perms::owner_all);
#endif
}

TEST_F(DataSourceCheckTest, ATestConnectionWritesNothing) {
  const DataSource missing = local_source("sub/new.db", "runs/new");
  {
    FakeOpeners fake;
    const auto before = hash_tree(dir);
    auto checked = check_data_source(change_for(missing), missing, fake.openers(), false);
    ASSERT_TRUE(checked) << checked.error().what;
    EXPECT_EQ(hash_tree(dir), before);
  }
  fs::create_directories(root / "runs");
  write_text(root / "old.db", "store");
  const DataSource existing = local_source("old.db", "runs");
  FakeOpeners fake;
  const auto before = hash_tree(dir);
  auto checked = check_data_source(change_for(existing), existing, fake.openers(), false);
  ASSERT_TRUE(checked) << checked.error().what;
  EXPECT_EQ(hash_tree(dir), before);
  ASSERT_EQ(fake.opened.size(), 1U);
  EXPECT_FALSE(fake.opened[0].second);
}

TEST_F(DataSourceCheckTest, AnInstrumentInstallIsSeeded) {
  write_text(root / "seed.toml", "# seed\n");
  const DataSource local = local_source("new.db");
  {
    FakeOpeners fake;
    const auto said = seed_data_source(change_for(local, "instrument"), local, fake.openers(),
                                       DataSourceCheck{.said = {}, .made_file = true, .opened = true});
    ASSERT_EQ(fake.seeded.size(), 1U);
    EXPECT_EQ(fake.seeded[0].seed_file, root / "seed.toml");
    EXPECT_EQ(fake.seeded[0].url, "sqlite:" + (root / "new.db").generic_string());
    EXPECT_TRUE(any_contains(said, "seeded 3 samples"));
  }
  const DataSource server = server_source(std::string("pw"));
  {
    FakeOpeners fake;
    const auto said = seed_data_source(change_for(server, "instrument"), server, fake.openers(),
                                       DataSourceCheck{.said = {}, .made_file = false, .opened = true});
    ASSERT_EQ(fake.seeded.size(), 1U);
    EXPECT_EQ(fake.seeded[0].url, "postgresql://ana:pw@db.lab:5432/pychron");
    EXPECT_FALSE(fake.seeded[0].migrate);
    EXPECT_FALSE(any_contains(said, "pw@"));
  }
  {
    FakeOpeners fake;  // --no-check: nothing opened
    const auto said = seed_data_source(change_for(server, "instrument"), server, fake.openers(),
                                       DataSourceCheck{.said = {}, .made_file = false, .opened = false});
    EXPECT_TRUE(fake.seeded.empty());
    EXPECT_TRUE(any_contains(said, "seed skipped: "));
  }
  {
    FakeOpeners fake;
    fake.seed_reply = fail(ErrorKind::Io, "seed failed\ndetail");
    const auto said = seed_data_source(change_for(server, "instrument"), server, fake.openers(),
                                       DataSourceCheck{.said = {}, .made_file = false, .opened = true});
    EXPECT_EQ(fake.seeded.size(), 1U);
    EXPECT_TRUE(contains(said, "seed skipped: seed failed"));
  }
  {
    FakeOpeners fake;
    const auto said = seed_data_source(change_for(local, "data_reduction"), local, fake.openers(),
                                       DataSourceCheck{.said = {}, .made_file = true, .opened = true});
    EXPECT_TRUE(fake.seeded.empty());
  }
}

TEST_F(DataSourceCheckTest, ASeedErrorDoesNotShowThePassword) {
  write_text(root / "seed.toml", "# seed\n");
  const DataSourceCheck opened{.said = {}, .made_file = false, .opened = true};
  {
    const DataSource given = server_source(std::string("given-pw"));
    FakeOpeners fake;
    fake.seed_reply = fail(ErrorKind::Io, "cannot seed postgresql://ana:given-pw@db.lab:5432/pychron\ndetail");
    const auto said = seed_data_source(change_for(given, "instrument"), given, fake.openers(), opened);
    ASSERT_EQ(fake.seeded.size(), 1U);
    EXPECT_TRUE(any_contains(said, "seed skipped: cannot seed postgresql://ana:"));
    EXPECT_FALSE(any_contains(said, "given-pw"));
  }
  write_text(root / ".pychron" / "credentials.toml", "[database]\npassword = \"kept-pw\"\n");
  const DataSource kept = server_source(std::nullopt);
  FakeOpeners fake;
  fake.seed_reply = fail(ErrorKind::Io, "cannot seed with ana:kept-pw");
  const auto said = seed_data_source(change_for(kept, "instrument"), kept, fake.openers(), opened);
  ASSERT_EQ(fake.seeded.size(), 1U);
  EXPECT_TRUE(any_contains(said, "seed skipped: cannot seed with ana:"));
  EXPECT_FALSE(any_contains(said, "kept-pw"));
}

TEST_F(DataSourceCheckTest, AnExistingLocalFileIsNotSeeded) {
  write_text(root / "seed.toml", "# seed\n");
  write_text(root / "old.db", "store");
  const DataSource to = local_source("old.db");
  FakeOpeners fake;
  const auto said = seed_data_source(change_for(to, "instrument"), to, fake.openers(),
                                     DataSourceCheck{.said = {}, .made_file = false, .opened = true});
  EXPECT_TRUE(fake.seeded.empty());
  EXPECT_TRUE(contains(said, "seed skipped: " + (root / "old.db").generic_string() +
                                 " existed; an existing store is not seeded"));
  EXPECT_TRUE(any_contains(said, "an existing store is not seeded"));
}
