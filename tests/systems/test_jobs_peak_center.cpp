// Peak center: the pure fit against scans whose expected results come from
// legacy pychron's own peak_detection.py (tests/data/scans/generate.py), and
// the runner on the job rig with a peak placed off the field table.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>

#include <toml++/toml.hpp>

#include "jobs_fakes.hpp"
#include "pychron/systems/jobs/job_runner.hpp"
#include "pychron/systems/jobs/peak_center.hpp"

using namespace pychron;
using namespace pychron::jobs;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kScans = std::filesystem::path(PYCHRON_TEST_DATA_DIR) / "scans";

std::vector<std::pair<double, double>> load_scan(const std::string& name) {
  std::ifstream in(kScans / (name + ".csv"));
  std::vector<std::pair<double, double>> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto comma = line.find(',');
    out.emplace_back(std::stod(line.substr(0, comma)), std::stod(line.substr(comma + 1)));
  }
  return out;
}

// The message without its numbers.
std::string words(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const bool numeric = std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == 'e' ||
                         ((c == '-' || c == '+') && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])));
    if (!numeric || (c == 'e' && (i == 0 || !std::isdigit(static_cast<unsigned char>(s[i - 1])))))
      out += c;
  }
  return out;
}

void expect_close(std::optional<double> actual, std::optional<double> expected, const std::string& what) {
  ASSERT_EQ(actual.has_value(), expected.has_value()) << what;
  if (!expected) return;
  EXPECT_NEAR(*actual, *expected, 1e-9 * std::max(1.0, std::fabs(*expected))) << what;
}

TEST(PeakFit, MatchesLegacyPychronOnEveryFixtureScan) {
  auto expected = toml::parse_file((kScans / "expected.toml").string());
  ASSERT_TRUE(expected) << expected.error().description();
  int n = 0;
  for (const auto& [key, node] : expected.table()) {
    const std::string name(key.str());
    const auto& e = *node.as_table();
    SCOPED_TRACE(name);
    ++n;
    const auto points = load_scan(name);
    ASSERT_FALSE(points.empty());
    auto fit = find_peak_center(points, PeakFitOptions{80, 1.0, true, false});
    if (auto err = e["error"].value<std::string>()) {
      ASSERT_FALSE(fit) << "legacy failed with: " << *err;
      // Same message; numbers print differently in Python, so compare the words.
      EXPECT_EQ(words(fit.error().what), words(*err)) << fit.error().what << " vs " << *err;
    } else {
      ASSERT_TRUE(fit) << fit.error().what;
      const auto& p = *e["peak"].as_table();
      expect_close(fit->low_x, p["low_x"].value<double>(), "low_x");
      expect_close(fit->center_x, p["center_x"].value<double>(), "center_x");
      expect_close(fit->high_x, p["high_x"].value<double>(), "high_x");
      expect_close(fit->low_y, p["low_y"].value<double>(), "low_y");
      expect_close(fit->center_y, p["center_y"].value<double>(), "center_y");
      expect_close(fit->high_y, p["high_y"].value<double>(), "high_y");
      expect_close(fit->max_x, p["max_x"].value<double>(), "max_x");
      expect_close(fit->max_y, p["max_y"].value<double>(), "max_y");
      expect_close(fit->resolution, e["resolution"].value<double>(), "resolution");
      expect_close(fit->resolving_power_low, e["resolving_power_low"].value<double>(), "resolving_power_low");
      expect_close(fit->resolving_power_high, e["resolving_power_high"].value<double>(), "resolving_power_high");
    }
    // Resolution is computed even where the 80% fit fails (pychron ignores the height there).
    expect_close(peak_resolution(points), e["resolution"].value<double>(), "peak_resolution");
  }
  EXPECT_GE(n, 12);
}

TEST(PeakFit, GuardsAndOptions) {
  EXPECT_FALSE(find_peak_center({}));
  EXPECT_FALSE(find_peak_center({{1, 2}, {2, std::nan("")}}));
  const auto small = load_scan("too_small");
  EXPECT_FALSE(find_peak_center(small));
  EXPECT_TRUE(find_peak_center(small, PeakFitOptions{80, 1.0, true, true}));  // ignore_max
  const auto notched = load_scan("notched_plateau");
  EXPECT_FALSE(find_peak_center(notched));
  auto no_flat = find_peak_center(notched, PeakFitOptions{80, 1.0, false, false});
  ASSERT_TRUE(no_flat);
  EXPECT_NEAR(no_flat->center_x, 5.0, 1e-9);
  // A narrower percent finds narrower edges.
  const auto flat = load_scan("flat_top");
  auto wide = find_peak_center(flat, PeakFitOptions{95, 1.0, true, false});
  auto narrow = find_peak_center(flat, PeakFitOptions{20, 1.0, true, false});
  ASSERT_TRUE(wide && narrow);
  EXPECT_GT(wide->high_x - wide->low_x, narrow->high_x - narrow->low_x);
}

TEST(PeakCenterConfig, ParsesNamedConfigs) {
  auto c = parse_peak_center_configs(R"(
[default]
isotope = "Ar40"
detector = "H1"
additional_detectors = ["AX"]
window = 0.02
step = 0.001
percent = 70
n_tries = 3
direction = "decrease"
integration_s = 0.5
baseline_timeout_s = 4
peak_shift_threshold = 0.01
update_table = false
propagate = false

[cdd]
isotope = "Ar36"
detector = "CDD"
)");
  ASSERT_TRUE(c) << c.error().what;
  ASSERT_EQ(c->size(), 2u);
  const auto& d = c->at("default");
  EXPECT_EQ(d.name, "default");
  EXPECT_EQ(d.additional_detectors, std::vector<std::string>{"AX"});
  EXPECT_DOUBLE_EQ(d.window, 0.02);
  EXPECT_EQ(d.n_tries, 3);
  EXPECT_FALSE(d.increase);
  EXPECT_EQ(d.integration, 500ms);
  EXPECT_EQ(d.baseline_timeout, 4s);
  EXPECT_FALSE(d.update_table);
  EXPECT_FALSE(d.propagate);
  EXPECT_DOUBLE_EQ(c->at("cdd").window, 0.015);  // defaults
  for (const char* bad : {"[x]\nbogus = 1\n", "[x]\nwindow = 0\n", "[x]\nstep = 0.1\n", "[x]\npercent = 100\n",
                          "[x]\nn_tries = 0\n", "[x]\ndirection = \"up\"\n", "[x]\nwindow = \"wide\"\n", "x = 1\n",
                          "[x]\nisotope = \"\"\n"})
    EXPECT_FALSE(parse_peak_center_configs(bad)) << bad;
}

// The rig's H1 signal is a flat-topped peak at `true_center` (native units) on
// whatever the magnet is set to; AX sees the same peak shifted by `ax_shift`.
class PeakCenterRun : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(rig_.spec);
    auto initial = rig_.spec->native_for(*rig_.spec->mass_of(spectrometer::PositionTarget{
                                             spectrometer::Isotope{"Ar40"}, "H1"}),
                                         "H1");
    ASSERT_TRUE(initial);
    initial_ = *initial;
    true_center_ = initial_ + 0.004;
    rig_.acquirer.signal = [this](const spectrometer::ChannelId& ch) {
      double c = true_center_;
      if (ch == "AX") c += ax_shift_;
      else if (ch != "H1") return 0.0;
      const double d = std::fabs(rig_.positioner.value - c);
      if (d <= 0.002) return height_;
      if (d <= 0.0035) return height_ * (1 - (d - 0.002) / 0.0015);
      return 0.0;
    };
  }

  Result<PeakCenterResult> run(PeakCenterConfig cfg, PeakCenterOptions options = {}) {
    if (!options.sweep.sleep) options.sweep.sleep = rig_.sweep_sleep();
    auto fut = std::async(std::launch::async, [&] { return run_peak_center(*rig_.spec, cfg, progress_, cancel_, options); });
    return rig_.drive(fut);
  }

  PeakCenterConfig config() {
    PeakCenterConfig c;
    c.detector = "H1";
    c.isotope = "Ar40";
    c.integration = 1s;
    return c;
  }

  JobRig rig_;
  Progress progress_;
  CancelToken cancel_;
  double initial_ = 0, true_center_ = 0, ax_shift_ = 0.001, height_ = 1000;
};

TEST_F(PeakCenterRun, CentersUpdatesTheTableAndRepositions) {
  auto cfg = config();
  cfg.additional_detectors = {"AX"};
  auto r = run(cfg);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->ok) << r->message;
  EXPECT_NEAR(r->initial_center, initial_, 1e-12);
  ASSERT_TRUE(r->center);
  EXPECT_NEAR(*r->center, true_center_, cfg.step);
  ASSERT_EQ(r->tries.size(), 1u);
  EXPECT_TRUE(r->tries[0].baseline_reached);
  EXPECT_EQ(r->tries[0].points.size(), 61u);  // +-0.015 at 0.0005
  ASSERT_TRUE(r->shape);
  EXPECT_TRUE(r->shape->resolution);
  ASSERT_EQ(r->additional.size(), 1u);
  ASSERT_TRUE(r->additional[0].shape);
  EXPECT_NEAR(r->additional[0].shape->center_x, true_center_ + ax_shift_, cfg.step);

  // The table now says the corrected center; positioning lands on it.
  EXPECT_TRUE(r->table_updated);
  EXPECT_NEAR(*r->table_value, *rig_.spec->uncorrect(*r->center, "H1"), 1e-12);
  EXPECT_NEAR(rig_.positioner.value, *r->center, 1e-9);
  auto again = rig_.spec->native_for(*rig_.spec->mass_of(spectrometer::PositionTarget{spectrometer::Isotope{"Ar40"}, "H1"}),
                                     "H1");
  EXPECT_NEAR(*again, *r->center, 1e-9);
}

TEST_F(PeakCenterRun, RetriesAroundTheMaximumWhenTheFirstWindowMissesThePeak) {
  true_center_ = initial_ + 0.018;  // the first scan only sees the peak's edge
  auto r = run(config());
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->ok) << r->message;
  ASSERT_EQ(r->tries.size(), 2u);
  EXPECT_NEAR(*r->center, true_center_, 0.0005);
  // Strong signal: the second window is centered on the first scan's maximum.
  EXPECT_NEAR((r->tries[1].start + r->tries[1].end) / 2, initial_ + 0.015, 1e-9);
}

TEST_F(PeakCenterRun, NoPeakFailsWithPointsKept) {
  height_ = 0.2;  // below min_peak_height
  auto r = run(config());
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_FALSE(r->ok);
  EXPECT_NE(r->message.find("No peak greater than 1"), std::string::npos) << r->message;
  EXPECT_EQ(r->tries.size(), 2u);
  for (const auto& t : r->tries) EXPECT_FALSE(t.points.empty());
  EXPECT_FALSE(r->table_updated);
  // The second window grows: window * (0.1 + 1) around the first scan's maximum.
  EXPECT_NEAR(std::fabs(r->tries[1].end - r->tries[1].start), 2 * 0.015 * 1.1, 1e-9);
}

TEST_F(PeakCenterRun, ShiftThresholdOffsetAndNoTableUpdate) {
  auto cfg = config();
  cfg.peak_shift_threshold = 0.002;  // the peak is 0.004 away
  cfg.n_tries = 1;
  auto r = run(cfg);
  ASSERT_TRUE(r);
  EXPECT_FALSE(r->ok);
  EXPECT_NE(r->message.find("moved too much"), std::string::npos) << r->message;

  cfg.peak_shift_threshold = 0;
  cfg.dac_offset = 0.0005;
  cfg.update_table = false;
  const auto table_before = rig_.spec->field_table();
  r = run(cfg);
  ASSERT_TRUE(r && r->ok);
  EXPECT_NEAR(*r->center, r->shape->center_x + 0.0005, 1e-12);
  EXPECT_FALSE(r->table_updated);
  EXPECT_NEAR(rig_.positioner.value, *r->center, 1e-9);  // still moved onto the center
  EXPECT_NEAR(*rig_.spec->native_for(*rig_.spec->mass_of(spectrometer::PositionTarget{spectrometer::Isotope{"Ar40"}, "H1"}),
                                     "H1"),
              initial_, 1e-12);  // table untouched
}

TEST_F(PeakCenterRun, ConfigAndCancelErrors) {
  auto cfg = config();
  cfg.detector = "IC9";
  auto bad = run(cfg);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);

  cancel_.cancel();
  auto c = run(config());
  ASSERT_FALSE(c);
  EXPECT_EQ(c.error().kind, ErrorKind::Cancelled);
}

TEST_F(PeakCenterRun, AsAJobPublishesTheResult) {
  std::vector<PeakCenterResult> done;
  auto sub = rig_.bus.subscribe<PeakCenterDone>([&](const PeakCenterDone& e) { done.push_back(e.result); });
  JobRunner runner(*rig_.spec, rig_.scheduler, rig_.bus, rig_.clock);
  PeakCenterOptions options;
  options.sweep.sleep = rig_.sweep_sleep();
  options.bus = &rig_.bus;
  auto fut = std::async(std::launch::async, [&] { return runner.run(peak_center_job(config(), options)); });
  auto job = rig_.drive(fut);
  ASSERT_TRUE(job) << job.error().what;
  EXPECT_EQ(job->kind, "peak_center");
  EXPECT_EQ(job->state, JobState::Succeeded);
  const auto* result = std::any_cast<PeakCenterResult>(&job->result);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->ok);
  ASSERT_EQ(done.size(), 1u);
  EXPECT_EQ(done[0].center, result->center);
  EXPECT_TRUE(job->before && job->after);
}

}  // namespace
