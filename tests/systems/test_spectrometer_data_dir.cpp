#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "pychron/systems/spectrometer/config_validate.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

namespace pychron::spectrometer::cfg {
namespace {

namespace fs = std::filesystem;

const fs::path kExamples{PYCHRON_EXAMPLE_CONFIGS_DIR};

std::string dump(const std::vector<config::Diagnostic>& ds) {
  std::string out;
  for (const auto& d : ds) out += config::to_string(d) + "\n";
  return out;
}

bool mentions(const std::vector<config::Diagnostic>& ds, std::string_view field, std::string_view text) {
  for (const auto& d : ds) {
    if (d.field == field && d.message.find(text) != std::string::npos) return true;
  }
  return false;
}

void write(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << text;
}

class TempDir : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("pychron_spec_cfg_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override { fs::remove_all(root_); }
  fs::path root_;
};

// ---- layout ---------------------------------------------------------------

TEST(SpectrometerDataDir, LayoutPaths) {
  const fs::path r = "/data/spectrometer";
  EXPECT_EQ(config_path(r), r / "spectrometer.toml");
  EXPECT_EQ(molecular_weights_path(r), r / "molecular_weights.toml");
  EXPECT_EQ(table_dir(r, "argon"), r / "tables" / "argon");
  EXPECT_EQ(table_current_pointer(r, "argon"), r / "tables" / "argon" / "current");
  EXPECT_EQ(profile_path(r, "tune"), r / "profiles" / "tune.toml");
}

using SpectrometerDataDirFs = TempDir;

TEST_F(SpectrometerDataDirFs, ResolveCurrentTableFollowsPointer) {
  write(root_ / "tables/argon/current", "2026-01-01T000000.toml\n");
  write(root_ / "tables/argon/2026-01-01T000000.toml", "");
  auto p = resolve_current_table(root_, "argon");
  ASSERT_TRUE(p) << to_string(p.error());
  EXPECT_EQ(*p, root_ / "tables/argon/2026-01-01T000000.toml");
}

TEST_F(SpectrometerDataDirFs, ResolveCurrentTableErrors) {
  auto missing = resolve_current_table(root_, "argon");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);

  write(root_ / "tables/argon/current", "gone.toml");
  auto dangling = resolve_current_table(root_, "argon");
  ASSERT_FALSE(dangling);
  EXPECT_NE(dangling.error().what.find("gone.toml"), std::string::npos);

  write(root_ / "tables/argon/current", "../../escape.toml");
  EXPECT_FALSE(resolve_current_table(root_, "argon"));
}

TEST_F(SpectrometerDataDirFs, ListProfilesSorted) {
  EXPECT_TRUE(list_profiles(root_).empty());
  write(root_ / "profiles/tune.toml", "");
  write(root_ / "profiles/imported.toml", "");
  write(root_ / "profiles/notes.txt", "");
  EXPECT_EQ(list_profiles(root_), (std::vector<std::string>{"imported", "tune"}));
}

// ---- table files ----------------------------------------------------------

constexpr std::string_view kTable = R"toml(
fit = "quadratic"
axis = "dac"

[[points]]
isotope = "Ar40"
mass = 39.962
H2 = 5.123
H1 = 5.001

[[points]]
isotope = "Ar36"
mass = 35.968
H1 = 4.505
)toml";

TEST(SpectrometerTableFile, ParsesPointsAndColumns) {
  auto r = load_table_from_string(kTable, "t.toml", "argon");
  ASSERT_TRUE(r.ok()) << dump(r.diagnostics);
  const auto& t = *r.table;
  EXPECT_EQ(t.name, "argon");
  EXPECT_EQ(t.fit, FitKind::Quadratic);
  EXPECT_EQ(t.axis, Axis::Dac);
  ASSERT_EQ(t.points.size(), 2u);
  EXPECT_EQ(t.points[0].isotope, "Ar40");
  EXPECT_DOUBLE_EQ(t.points[0].values.at("H2"), 5.123);
  // H2 is not in every point, so it is not a usable column.
  EXPECT_EQ(t.columns(), (std::vector<std::string>{"H1"}));
  EXPECT_TRUE(t.has_column("H1"));
  EXPECT_FALSE(t.has_column("H2"));
}

TEST(SpectrometerTableFile, SchemaErrors) {
  auto r = load_table_from_string("fit = \"spline\"\naxis = \"volts\"\n[[points]]\nmass = \"x\"\nH1 = true\n", "t.toml",
                                  "argon");
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(mentions(r.diagnostics, "fit", "must be one of")) << dump(r.diagnostics);
  EXPECT_TRUE(mentions(r.diagnostics, "axis", "must be one of")) << dump(r.diagnostics);
  EXPECT_TRUE(mentions(r.diagnostics, "points[0].isotope", "missing required field")) << dump(r.diagnostics);
  EXPECT_TRUE(mentions(r.diagnostics, "points[0].mass", "expected number")) << dump(r.diagnostics);
  EXPECT_TRUE(mentions(r.diagnostics, "points[0].H1", "expected number")) << dump(r.diagnostics);

  auto empty = load_table_from_string("fit = \"linear\"\naxis = \"dac\"\n", "t.toml", "argon");
  EXPECT_TRUE(mentions(empty.diagnostics, "points", "at least one point"));
}

// ---- molecular weights ----------------------------------------------------

TEST(SpectrometerMolecularWeights, DefaultsCoverArgon) {
  const auto& w = default_molecular_weights();
  EXPECT_NEAR(w.at("Ar40"), 39.9624, 1e-4);
  EXPECT_NEAR(w.at("Ar36"), 35.9675, 1e-4);
  EXPECT_TRUE(w.contains("He4"));
  EXPECT_TRUE(w.contains("Xe132"));
}

TEST(SpectrometerMolecularWeights, FileExtendsAndOverridesDefaults) {
  auto r = load_molecular_weights_from_string("Ar40 = 40.0\nHCl35 = 35.976678\n", "mw.toml");
  ASSERT_TRUE(r.ok()) << dump(r.diagnostics);
  EXPECT_DOUBLE_EQ(r.weights->at("Ar40"), 40.0);
  EXPECT_DOUBLE_EQ(r.weights->at("HCl35"), 35.976678);
  EXPECT_TRUE(r.weights->contains("Ar36"));
}

TEST(SpectrometerMolecularWeights, RejectsNonPositiveAndNonNumbers) {
  auto r = load_molecular_weights_from_string("Ar40 = -1\nAr39 = \"heavy\"\n", "mw.toml");
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(mentions(r.diagnostics, "Ar40", "must be > 0")) << dump(r.diagnostics);
  EXPECT_TRUE(mentions(r.diagnostics, "Ar39", "expected number")) << dump(r.diagnostics);
}

// ---- shipped examples: the whole pipeline ----------------------------------

TEST(SpectrometerExamples, SimIntegratedLoadsAndValidates) {
  auto r = load_spectrometer_data(kExamples / "spectrometer.sim-integrated.toml");
  ASSERT_TRUE(r.ok()) << dump(r.diagnostics);
  const auto& d = *r.data;
  EXPECT_EQ(d.root, kExamples);
  EXPECT_EQ(d.config.drivers.size(), 1u);
  EXPECT_EQ(d.config.drivers.at("sim").roles.size(), 5u);
  EXPECT_FALSE(d.config.acquisition.host_integration);
  EXPECT_TRUE(d.config.detector_control);
  EXPECT_EQ(d.config.detectors.size(), 6u);
  EXPECT_TRUE(d.tables.contains("argon"));
  EXPECT_TRUE(d.tables.contains("argon_hv"));
  EXPECT_DOUBLE_EQ(d.weights.at("HCl35"), 35.976678);
}

TEST(SpectrometerExamples, SimLegacyLoadsAndValidates) {
  auto r = load_spectrometer_data(kExamples / "spectrometer.sim-legacy.toml");
  ASSERT_TRUE(r.ok()) << dump(r.diagnostics);
  const auto& c = r.data->config;
  EXPECT_EQ(c.drivers.size(), 4u);
  EXPECT_EQ(c.acquisition.acquirers.size(), 2u);
  EXPECT_TRUE(c.acquisition.host_integration);
  EXPECT_FALSE(c.detector_control);
  EXPECT_TRUE(r.data->tables.contains("argon_legacy"));
}

TEST(SpectrometerExamples, ResultFormOk) {
  auto r = load_spectrometer(kExamples / "spectrometer.sim-legacy.toml");
  ASSERT_TRUE(r) << to_string(r.error());
}

TEST_F(SpectrometerDataDirFs, LoadCollectsTableAndRuleErrors) {
  // Legacy example with its table removed and one rule broken.
  std::ifstream in(kExamples / "spectrometer.sim-legacy.toml");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  text.replace(text.find("native_axis = \"dac\""), 19, "native_axis = \"mass\"");
  write(root_ / "spectrometer.toml", text);

  auto r = load_spectrometer_data(root_ / "spectrometer.toml");
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(mentions(r.diagnostics, "magnet.field_table", "not found")) << dump(r.diagnostics);
  EXPECT_TRUE(mentions(r.diagnostics, "magnet.corrections.hv", "mass")) << dump(r.diagnostics);

  auto res = load_spectrometer(root_ / "spectrometer.toml");
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error().kind, ErrorKind::Config);
}

TEST_F(SpectrometerDataDirFs, LocalOverrideIsMerged) {
  std::ifstream in(kExamples / "spectrometer.sim-legacy.toml");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  write(root_ / "spectrometer.toml", text);
  write(root_ / "spectrometer.local.toml", "[transports.hv]\nport = \"/dev/tty.usbserial-X\"\n");
  fs::copy(kExamples / "tables", root_ / "tables", fs::copy_options::recursive);

  auto r = load_spectrometer_data(root_ / "spectrometer.toml");
  ASSERT_TRUE(r.ok()) << dump(r.diagnostics);
  EXPECT_EQ(r.data->config.transports.at("hv").serial_port, "/dev/tty.usbserial-X");
  // No molecular_weights.toml here: defaults are used.
  EXPECT_TRUE(r.data->weights.contains("Ar40"));
  EXPECT_FALSE(r.data->weights.contains("HCl35"));
}

}  // namespace
}  // namespace pychron::spectrometer::cfg
