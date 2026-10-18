// Changing an install's data source (data source editing spec, 3.1): the
// current source read from the install record or the site entry, the URL it
// stands for, and a planned change, refused before anything is written.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "pychron/setup/data_source.hpp"
#include "pychron/setup/install.hpp"
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
