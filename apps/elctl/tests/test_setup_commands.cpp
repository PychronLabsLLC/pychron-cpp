// elctl init / doctor / --install against the shipped profiles, with a site
// config of the test's own (the fixture sets PYCHRON_SITE_CONFIG).

#include <filesystem>
#include <fstream>
#include <string>

#include "elctl_fixture.hpp"
#include "pychron/setup/site.hpp"

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
  EXPECT_TRUE(contains(o.out, "queue first-queue: 3 run(s)")) << o.out;
}

TEST_F(ElctlSetupTest, QuestionsAreAskedWithDefaultsInBrackets) {
  const auto root = path("ngx-lab");
  // simulation: no; reference isotope, baseline mass, peak window, host: Enter
  // (defaults); port: 1091; user, reference detector: Enter; then confirm.
  auto o = run_raw({"init", "ngx", "--root", root.string()}, "no\n\n\n\n\n1091\n\n\ny\n");
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

}  // namespace
}  // namespace elctl::testing
