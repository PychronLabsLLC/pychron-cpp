// Snapshots (live camera design, section 5): a laser system with a camera
// saves what it sees.

#include <gtest/gtest.h>

#include "laser_harness.hpp"
#include "pychron/vision/pgm.hpp"

using namespace pychron;
using namespace pychron::laser;
using namespace pychron::laser::harness;

namespace {

struct Snapshots : ::testing::Test, CameraHarness {
  fs::path dir = lab.dir / "snapshots" / "co2";
  Snapshots() { system.set_snapshot_dir(dir); }
  IImaging& imaging() { return *system.imaging(); }
  static bool is_png(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char magic[8] = {};
    in.read(magic, 8);
    return in.gcount() == 8 && std::string(magic, 8) == "\x89PNG\r\n\x1a\n";
  }
};

}  // namespace

TEST_F(Snapshots, SavesThePictureUnderTheNameGiven) {
  ASSERT_NE(system.imaging(), nullptr);
  const auto saved = imaging().snapshot("before");
  ASSERT_TRUE(saved) << saved.error().what;
  EXPECT_EQ(fs::path(*saved), dir / "before.png");
  EXPECT_TRUE(is_png(dir / "before.png"));
  EXPECT_GT(fs::file_size(dir / "before.png"), static_cast<std::uintmax_t>(camera.sim_width * camera.sim_height));
  // an extension given is not doubled
  EXPECT_EQ(fs::path(*imaging().snapshot("after.png")), dir / "after.png");
}

TEST_F(Snapshots, NeverWritesOverAPicture) {
  ASSERT_TRUE(imaging().snapshot("hole3"));
  const auto second = imaging().snapshot("hole3");
  ASSERT_TRUE(second) << second.error().what;
  EXPECT_EQ(fs::path(*second), dir / "hole3-2.png");
  EXPECT_EQ(fs::path(*imaging().snapshot("hole3")), dir / "hole3-3.png");
}

TEST_F(Snapshots, WithNoNameItIsTheTime) {
  const auto saved = imaging().snapshot("");
  ASSERT_TRUE(saved) << saved.error().what;
  const std::string stem = fs::path(*saved).stem().string();
  ASSERT_EQ(stem.size(), 15u) << stem;  // 20261005-142233
  EXPECT_EQ(stem[8], '-');
  EXPECT_TRUE(is_png(*saved));
}

// The time is the clock's calendar time (here a ManualClock whose epoch is
// 1970), so a simulated session names its pictures in simulated time.
TEST_F(Snapshots, TheTimeIsTheClocks) {
  clock.advance(1h + 2min + 3s);
  const auto saved = imaging().snapshot("");
  ASSERT_TRUE(saved) << saved.error().what;
  EXPECT_EQ(fs::path(*saved).stem().string(), "19700101-010203");
}

TEST_F(Snapshots, ANameMayNotLeaveTheDirectory) {
  for (const char* name : {"../out", "a/b", "..", ".hidden", "c:\\x"}) {
    const auto saved = imaging().snapshot(name);
    ASSERT_FALSE(saved) << name;
    EXPECT_EQ(saved.error().kind, ErrorKind::Config);
  }
  EXPECT_FALSE(fs::exists(lab.dir / "snapshots" / "out.png"));
  EXPECT_FALSE(fs::exists(dir)) << "nothing was made for a name that was refused";
}

TEST_F(Snapshots, RecordingIsNotSupported) {
  const auto started = imaging().start_video_recording("x");
  ASSERT_FALSE(started);
  EXPECT_TRUE(extraction::is_not_supported(started.error()));
  EXPECT_FALSE(imaging().stop_video_recording());
}

TEST(SnapshotsWithout, NoCameraNoImaging) {
  LaserHarness h;
  h.system.set_snapshot_dir(h.lab.dir / "snapshots");
  EXPECT_EQ(h.system.imaging(), nullptr);
}

TEST(SnapshotsWithout, NoDirectoryIsSaid) {
  CameraHarness h;
  ASSERT_NE(h.system.imaging(), nullptr);
  const auto saved = h.system.imaging()->snapshot("x");
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().kind, ErrorKind::Config);
}
