#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pychron/vision/finder.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/pgm.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron;
using namespace pychron::vision;

namespace {

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int n = 0;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path = std::filesystem::temp_directory_path() /
           (std::string("pychron_vision_") + info->test_suite_name() + "_" + info->name() + "_" + std::to_string(++n));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void write_text(const std::filesystem::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

// Runs the finder on every non-skipped, marked frame of every case below `root`.
// Returns the number of frames checked.
int check_cases(const std::filesystem::path& root) {
  int checked = 0;
  std::vector<std::filesystem::path> dirs;
  for (const auto& e : std::filesystem::directory_iterator(root))
    if (e.is_directory() && std::filesystem::exists(e.path() / "case.toml")) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& dir : dirs) {
    auto c = load_case(dir);
    EXPECT_TRUE(c.has_value()) << dir << ": " << (c ? "" : c.error().what);
    if (!c) continue;
    for (const auto& ff : c->frames) {
      if (ff.skip || !ff.center_px) continue;
      auto fr = read_pgm(c->dir / ff.file);
      EXPECT_TRUE(fr.has_value()) << c->dir / ff.file;
      if (!fr) continue;
      FinderParams p;
      p.mode = c->mode;
      p.expected_radius_px = c->expected_radius_px;
      p.mask_radius_px = std::min(fr->width, fr->height) / 2.0;
      const auto targets = SimpleFinder{}.find(fr->view(), p);
      EXPECT_FALSE(targets.empty()) << c->dir.filename() << "/" << ff.file << ": no target found";
      if (targets.empty()) continue;
      const double err = std::hypot(targets[0].center_px.x - ff.center_px->x, targets[0].center_px.y - ff.center_px->y);
      EXPECT_LE(err, c->tolerance_px) << c->dir.filename() << "/" << ff.file << ": centre error " << err << " px";
      ++checked;
    }
  }
  return checked;
}

}  // namespace

TEST(Fixture, RecorderWritesLoadableCase) {
  TempDir d;
  const auto dir = d.path / "case";
  FrameRecorder rec(dir, Provenance::Synthetic, FinderMode::Hole, 11.5);
  HoleScene scene;
  scene.width = 40;
  scene.height = 30;
  std::vector<Frame> frames;
  for (int i = 0; i < 3; ++i) {
    frames.push_back(render(scene, {0.1 * i, 0.0}).first);
    ASSERT_TRUE(rec.add(frames.back().view(), i == 1 ? std::optional<Vec2>{} : std::optional<Vec2>{Vec2{20.0 + i, 14.5}})
                    .has_value());
  }
  ASSERT_TRUE(rec.finish().has_value());

  auto c = load_case(dir);
  ASSERT_TRUE(c.has_value()) << c.error().what;
  EXPECT_EQ(c->provenance, Provenance::Synthetic);
  EXPECT_EQ(c->mode, FinderMode::Hole);
  EXPECT_DOUBLE_EQ(c->expected_radius_px, 11.5);
  EXPECT_EQ(c->channel, "luma");
  ASSERT_EQ(c->frames.size(), 3U);
  EXPECT_EQ(c->frames[0].file, "0001.pgm");
  EXPECT_EQ(c->frames[2].file, "0003.pgm");
  ASSERT_TRUE(c->frames[0].center_px.has_value());
  EXPECT_DOUBLE_EQ(c->frames[2].center_px->x, 22.0);
  EXPECT_FALSE(c->frames[1].center_px.has_value());

  RecordedSource src(*c);
  const auto info = src.info();
  EXPECT_EQ(info.width, 40);
  EXPECT_EQ(info.height, 30);
  EXPECT_EQ(info.pixel_depth, 255);
  EXPECT_EQ(info.fps, 0.0);
  for (int i = 0; i < 3; ++i) {
    auto f = src.grab();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->seq, static_cast<std::uint64_t>(i + 1));
    EXPECT_EQ(f->data, frames[static_cast<std::size_t>(i)].data);
  }
  auto end = src.grab();
  ASSERT_FALSE(end.has_value());
  EXPECT_EQ(end.error().kind, ErrorKind::Io);
  EXPECT_EQ(end.error().what, "end of recording");
}

TEST(Fixture, ChannelNoteAndSkipRoundTrip) {
  TempDir d;
  FixtureCase c;
  c.dir = d.path;
  c.provenance = Provenance::ScreenRecording;
  c.mode = FinderMode::Glow;
  c.expected_radius_px = 9;
  c.tolerance_px = 4;
  c.channel = "b";
  c.note = "from \"x.mov\" at 0:35";
  c.frames.push_back(FixtureFrame{"0001.pgm", Vec2{10.5, 20}, true});
  write_text(d.path / "0001.pgm", "x");
  ASSERT_TRUE(save_case(c).has_value());
  auto r = load_case(d.path);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  EXPECT_EQ(r->provenance, Provenance::ScreenRecording);
  EXPECT_EQ(r->mode, FinderMode::Glow);
  EXPECT_EQ(r->channel, "b");
  EXPECT_EQ(r->note, c.note);
  ASSERT_EQ(r->frames.size(), 1U);
  EXPECT_TRUE(r->frames[0].skip);
  EXPECT_DOUBLE_EQ(r->frames[0].center_px->x, 10.5);
}

TEST(Fixture, LoadCaseRejectsMissingFrameFile) {
  TempDir d;
  write_text(d.path / "case.toml",
             "provenance = \"raw\"\nmode = \"hole\"\nexpected_radius_px = 10\ntolerance_px = 2\n"
             "[[frames]]\nfile = \"gone.pgm\"\n");
  auto r = load_case(d.path);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("gone.pgm"), std::string::npos);
}

TEST(Fixture, LoadCaseRejectsUnknownProvenanceOrMode) {
  TempDir d;
  write_text(d.path / "case.toml",
             "provenance = \"photo\"\nmode = \"hole\"\nexpected_radius_px = 10\ntolerance_px = 2\nframes = []\n");
  auto a = load_case(d.path);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().kind, ErrorKind::Config);

  write_text(d.path / "case.toml",
             "provenance = \"raw\"\nmode = \"ring\"\nexpected_radius_px = 10\ntolerance_px = 2\nframes = []\n");
  auto b = load_case(d.path);
  ASSERT_FALSE(b.has_value());
  EXPECT_EQ(b.error().kind, ErrorKind::Config);
}

TEST(Fixture, CommittedCasesWithinTolerance) {
  const int n = check_cases(PYCHRON_VISION_DATA_DIR);
  EXPECT_GT(n, 0) << "no committed frames were checked";
}

TEST(Fixture, ExternalCasesWithinTolerance) {
  const char* env = std::getenv("PYCHRON_VISION_FIXTURES");
  if (env == nullptr || *env == '\0') GTEST_SKIP() << "PYCHRON_VISION_FIXTURES is not set";
  check_cases(env);
}
