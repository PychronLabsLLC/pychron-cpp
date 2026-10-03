// Installing a profile: two phases (nothing written when anything fails),
// never overwriting, reconfigure rewriting only untouched files, the install
// record, secrets, answers and manifest errors, and the site config.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"

using namespace pychron;
using namespace pychron::setup;
namespace fs = std::filesystem;

namespace {

struct Tmp {
  fs::path dir = fs::temp_directory_path() /
                 ("pychron-setup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Tmp() { fs::create_directories(dir); }
  ~Tmp() { fs::remove_all(dir); }
  void write(const fs::path& rel, const std::string& text) const {
    fs::create_directories((dir / rel).parent_path());
    std::ofstream(dir / rel, std::ios::binary) << text;
  }
  std::string read(const fs::path& rel) const {
    std::ifstream in(dir / rel, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

// A small profile library in a scratch directory.
struct Fixture : ::testing::Test {
  Tmp profiles, examples, root;

  void SetUp() override {
    profiles.write("base/profile.toml", R"(
name = "base"
kind = "fragment"
[[files]]
copy = "@examples/shared.txt"
to = "shared.txt"
)");
    examples.write("shared.txt", "shared\n");
    profiles.write("app/profile.toml", R"(
name = "app"
kind = "instrument"
version = 2
includes = ["base"]
[values]
family = "test"
[[questions]]
id = "host"
type = "host"
default = "10.0.0.1"
[[questions]]
id = "remote"
type = "bool"
default = false
[[questions]]
id = "token"
type = "secret"
when = "remote"
[[files]]
template = "main.toml"
to = "main.toml"
[[files]]
template = "secret.toml"
to = "secret.toml"
when = "remote"
secret = true
)");
    profiles.write("app/main.toml", "family = \"{{ family }}\"\nhost = {{ host | toml }}\n");
    profiles.write("app/secret.toml", "token = {{ token | toml }}\n");
  }

  ProfileLibrary lib() {
    auto l = ProfileLibrary::load(profiles.dir, examples.dir);
    EXPECT_TRUE(l) << (l ? "" : l.error().what);
    return std::move(*l);
  }

  Result<InstallPlan> plan(const Answers& given, PlanOptions options = {}) {
    auto l = lib();
    auto r = l.resolve("app");
    if (!r) return fail(r.error());
    auto a = complete_answers(*r, given);
    if (!a) return fail(a.error());
    return plan_install(l, *r, *a, root.dir / "lab", options);
  }
};

}  // namespace

TEST_F(Fixture, InstallsOnceThenFindsEverythingInPlace) {
  auto p = plan({});
  ASSERT_TRUE(p) << p.error().what;
  ASSERT_EQ(p->files.size(), 2u);  // shared.txt and main.toml; secret.toml only when remote
  auto r = apply_install(*p);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->written.size(), 2u);
  EXPECT_EQ(root.read("lab/main.toml"), "family = \"test\"\nhost = \"10.0.0.1\"\n");
  auto record = read_install_record(root.dir / "lab");
  ASSERT_TRUE(record) << record.error().what;
  EXPECT_EQ(record->profile, "app");
  EXPECT_EQ(record->versions.at("app"), 2);
  EXPECT_EQ(record->files.size(), 2u);
  EXPECT_EQ(to_text(record->answers.at("host")), "10.0.0.1");

  auto again = plan({});
  ASSERT_TRUE(again);
  for (const auto& f : again->files) EXPECT_EQ(f.action, PlannedFile::Action::Same) << f.to;
}

TEST_F(Fixture, AnInstallNeverOverwritesAndAReconfigureOnlyRewritesUntouchedFiles) {
  ASSERT_TRUE(apply_install(*plan({})));
  root.write("lab/shared.txt", "edited by the lab\n");
  // A plain install over it keeps the edit.
  auto keep = plan({{"host", Value{std::string("10.0.0.2")}}});
  ASSERT_TRUE(keep);
  for (const auto& f : keep->files) {
    if (f.to == "shared.txt" || f.to == "main.toml") {
      EXPECT_EQ(f.action, PlannedFile::Action::Keep) << f.to;
    }
  }
  // A reconfigure rewrites main.toml (untouched) and puts shared.txt.new beside the edit.
  auto re = plan({{"host", Value{std::string("10.0.0.2")}}}, PlanOptions{true, false});
  ASSERT_TRUE(re) << re.error().what;
  auto report = apply_install(*re);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->updated, std::vector<fs::path>{"main.toml"});
  EXPECT_EQ(report->conflicts, std::vector<fs::path>{"shared.txt.new"});
  EXPECT_EQ(root.read("lab/shared.txt"), "edited by the lab\n");
  EXPECT_EQ(root.read("lab/shared.txt.new"), "shared\n");
  EXPECT_NE(root.read("lab/main.toml").find("10.0.0.2"), std::string::npos);
}

TEST_F(Fixture, SecretsAreOwnerOnlyAndNeverRecorded) {
  auto p = plan({{"remote", Value{true}}, {"token", Value{std::string("t0ken")}}});
  ASSERT_TRUE(p) << p.error().what;
  ASSERT_TRUE(apply_install(*p));
  EXPECT_EQ(root.read("lab/secret.toml"), "token = \"t0ken\"\n");
#ifndef _WIN32
  EXPECT_EQ(fs::status(root.dir / "lab/secret.toml").permissions() & (fs::perms::group_all | fs::perms::others_all),
            fs::perms::none);
#endif
  EXPECT_EQ(root.read("lab/.pychron/install.toml").find("t0ken"), std::string::npos);
  auto record = read_install_record(root.dir / "lab");
  ASSERT_TRUE(record);
  EXPECT_FALSE(record->answers.count("token"));
  // A reconfigure without the secret keeps the file it has.
  auto re = plan({{"remote", Value{true}}, {"token", Value{std::string{}}}}, PlanOptions{true, true});
  ASSERT_TRUE(re) << re.error().what;
  for (const auto& f : re->files) EXPECT_NE(f.to, fs::path("secret.toml"));
}

TEST_F(Fixture, NothingIsWrittenWhenARenderedTomlDoesNotParse) {
  profiles.write("app/main.toml", "host = {{ host }}\n");  // unquoted: not TOML
  auto p = plan({});
  ASSERT_FALSE(p);
  EXPECT_NE(p.error().what.find("main.toml: the rendered file does not parse"), std::string::npos) << p.error().what;
  EXPECT_FALSE(fs::exists(root.dir / "lab"));
}

TEST_F(Fixture, AnswersAreTypedAndChecked) {
  auto l = lib();
  auto r = l.resolve("app");
  ASSERT_TRUE(r);
  auto bad = complete_answers(*r, {{"host", Value{std::string("not a host!")}},
                                   {"nope", Value{std::string("1")}},
                                   {"remote", Value{std::string("yes")}}});
  ASSERT_FALSE(bad);
  for (const char* expected : {"host: \"not a host!\" is not a host", "nope: not a question", "token: needs an answer"})
    EXPECT_NE(bad.error().what.find(expected), std::string::npos) << bad.error().what;
  // Text from --set is typed by the question.
  auto ok = complete_answers(*r, {{"remote", Value{std::string("yes")}}, {"token", Value{std::string("x")}}});
  ASSERT_TRUE(ok) << ok.error().what;
  EXPECT_TRUE(std::get<bool>(ok->at("remote")));
  EXPECT_EQ(to_text(ok->at("family")), "test");  // [values] are answers too
}

TEST_F(Fixture, ManifestMistakesAreReported) {
  profiles.write("loop/profile.toml", "name = \"loop\"\nkind = \"instrument\"\nincludes = [\"loop\"]\n");
  profiles.write("lost/profile.toml", "name = \"lost\"\nkind = \"instrument\"\nincludes = [\"nowhere\"]\n");
  auto l = lib();
  EXPECT_NE(l.resolve("loop").error().what.find("includes itself"), std::string::npos);
  EXPECT_NE(l.resolve("lost").error().what.find("unknown profile 'nowhere'"), std::string::npos);
  profiles.write("bad/profile.toml", R"(
name = "wrong"
kind = "gadget"
colour = 1
[[files]]
to = "../escape.txt"
[[questions]]
id = "p"
type = "choice"
)");
  auto broken = ProfileLibrary::load(profiles.dir, examples.dir);
  ASSERT_FALSE(broken);
  for (const char* expected : {"must match the directory name", "kind: must be", "colour: unknown key",
                               "needs exactly one of template and copy", "must stay inside the install root",
                               "a choice needs choices"})
    EXPECT_NE(broken.error().what.find(expected), std::string::npos) << expected << "\n" << broken.error().what;
}

TEST(Site, SavesLoadsAndPicks) {
  Tmp tmp;
  const fs::path path = tmp.dir / "site.toml";
  auto empty = load_site(path);
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->pick(), nullptr);
  SiteConfig site;
  site.upsert({"argus", "instrument", "argus", tmp.dir / "argus", "extraction_line.toml", "canvas.toml",
               "spectrometer.toml", "data", "", true});
  EXPECT_EQ(site.pick()->name, "argus");  // the only one
  site.upsert({"reduction", "data_reduction", "data-reduction", tmp.dir / "dr", "", "", "", "data", "sqlite:/x.db", false});
  EXPECT_EQ(site.pick(), nullptr);  // two and no default
  site.default_install = "reduction";
  ASSERT_TRUE(save_site(site, path));
  auto loaded = load_site(path);
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_EQ(loaded->pick()->name, "reduction");
  EXPECT_EQ(loaded->find("argus")->simulation, true);
  EXPECT_EQ(loaded->find("argus")->path("spectrometer.toml"), tmp.dir / "argus" / "spectrometer.toml");
  EXPECT_EQ(loaded->find("reduction")->database, "sqlite:/x.db");
  EXPECT_TRUE(loaded->remove("reduction"));
  EXPECT_TRUE(loaded->default_install.empty());
}

TEST(Site, TheLocationCanBeOverridden) {
#ifdef _WIN32
  _putenv_s("PYCHRON_SITE_CONFIG", "C:/somewhere/site.toml");
  EXPECT_EQ(default_site_path(), fs::path("C:/somewhere/site.toml"));
  _putenv_s("PYCHRON_SITE_CONFIG", "");
#else
  setenv("PYCHRON_SITE_CONFIG", "/somewhere/site.toml", 1);
  EXPECT_EQ(default_site_path(), fs::path("/somewhere/site.toml"));
  unsetenv("PYCHRON_SITE_CONFIG");
#endif
  EXPECT_EQ(default_site_path().filename(), "site.toml");
}

TEST(Installer, DatabaseUrlsFromTheAnswers) {
  const Answers local{{"data_source", Value{std::string("local")}}};
  EXPECT_EQ(database_url_for(local, "/labs/dr", true), "sqlite:/labs/dr/data/pychron.db");
  Answers server{{"data_source", Value{std::string("server")}}, {"db_user", Value{std::string("ar user")}},
                 {"db_host", Value{std::string("db.lab.edu")}}, {"db_port", Value{std::int64_t{5433}}},
                 {"db_name", Value{std::string("pychron")}}, {"db_password", Value{std::string("p@ss:/")}}};
  EXPECT_EQ(database_url_for(server, "/x", false), "postgresql://ar%20user@db.lab.edu:5433/pychron");
  EXPECT_EQ(database_url_for(server, "/x", true), "postgresql://ar%20user:p%40ss%3A%2F@db.lab.edu:5433/pychron");
  server["db_password"] = Value{std::string{}};
  EXPECT_EQ(database_url_for(server, "/x", true), "postgresql://ar%20user@db.lab.edu:5433/pychron");
}

TEST(Installer, RegisteringAnInstallMakesTheFirstOneTheDefault) {
  Tmp tmp;
  const fs::path site = tmp.dir / "site.toml";
  ASSERT_TRUE(register_install({"a", "instrument", "argus", tmp.dir / "a", "", "", "", "data", "", true}, site));
  ASSERT_TRUE(register_install({"b", "instrument", "ngx", tmp.dir / "b", "", "", "", "data", "", true}, site));
  auto loaded = load_site(site);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->default_install, "a");
  EXPECT_EQ(loaded->installs.size(), 2u);
}

TEST(Installer, TheShippedProfilesAreFoundAndEnvironmentWins) {
  const Resources r = find_resources();
  EXPECT_TRUE(fs::exists(r.profiles / "data-reduction" / "profile.toml")) << r.profiles;
  EXPECT_TRUE(fs::exists(r.examples / "extraction_line.toml")) << r.examples;
  // An installed layout next to the program wins over the source tree.
  Tmp tmp;
  fs::create_directories(tmp.dir / "share" / "pychron" / "profiles");
  fs::create_directories(tmp.dir / "share" / "pychron" / "examples");
  fs::create_directories(tmp.dir / "bin");
  EXPECT_EQ(find_resources(tmp.dir / "bin").profiles, fs::weakly_canonical(tmp.dir / "share" / "pychron") / "profiles");
}

TEST(Installer, ChoiceLabelsMatchTheChoices) {
  Tmp profiles, examples;
  profiles.write("c/profile.toml", R"(
name = "c"
kind = "instrument"
[[questions]]
id = "where"
type = "choice"
choices = ["a", "b"]
labels = ["only one"]
)");
  auto bad = ProfileLibrary::load(profiles.dir, examples.dir);
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().what.find("needs one label per choice"), std::string::npos) << bad.error().what;
  auto shipped = ProfileLibrary::load(find_resources().profiles, find_resources().examples);
  ASSERT_TRUE(shipped) << shipped.error().what;
  auto dr = shipped->resolve("data-reduction");
  ASSERT_TRUE(dr);
  for (const auto& q : dr->questions)
    if (q.id == "data_source") {
      EXPECT_EQ(q.labels.size(), 2u);
    }
}
