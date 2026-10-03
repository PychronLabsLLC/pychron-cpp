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
  // Simulation, then the connection, then the detectors: simulation: no;
  // host: Enter (default); port: 1091; user, reference isotope, baseline
  // mass, peak window, reference detector, extraction line: Enter; then confirm.
  auto o = run_raw({"init", "ngx", "--root", root.string()}, "no\n\n1091\n\n\n\n\n\n\ny\n");
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

}  // namespace
}  // namespace elctl::testing
