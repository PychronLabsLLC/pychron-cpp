// OpenCvSource happy path. The test video is written with OpenCV, so everything here
// except the build-flag check sits behind PYCHRON_VISION_OPENCV_ENABLED.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

#include "pychron/vision/legacy_finder.hpp"
#include "pychron/vision/opencv_source.hpp"

#ifdef PYCHRON_VISION_OPENCV_ENABLED
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#endif

using namespace pychron::vision;

#ifdef PYCHRON_VISION_OPENCV_ENABLED

namespace {

constexpr int kW = 64, kH = 48, kFrames = 3;
constexpr double kTol = 25;  // lossy codec: compare quadrant means, with slack

// Source quadrants in reading order: top-left, top-right, bottom-left, bottom-right.
// Distinct in every channel; stored as {R, G, B}.
constexpr std::array<std::array<int, 3>, 4> kColours{{{200, 30, 30}, {30, 200, 30}, {30, 30, 200}, {200, 200, 30}}};
// Rec. 601 luma of the above: 81, 130, 49, 181 (so the order is BR > TR > TL > BL).
constexpr std::array<double, 4> kLuma{81, 130, 49, 181};

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

// Writes kFrames identical frames; false when this machine has no writer for the codec.
bool write_video(const std::filesystem::path& file) {
  cv::VideoWriter w(file.string(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 10.0, cv::Size(kW, kH), true);
  if (!w.isOpened()) return false;
  cv::Mat m(kH, kW, CV_8UC3);
  for (int q = 0; q < 4; ++q) {
    const cv::Rect r((q % 2) * kW / 2, (q / 2) * kH / 2, kW / 2, kH / 2);
    // OpenCV stores B, G, R.
    m(r).setTo(cv::Scalar(kColours[q][2], kColours[q][1], kColours[q][0]));
  }
  for (int i = 0; i < kFrames; ++i) w.write(m);
  return true;
}

// Mean of the middle half of a quadrant (away from codec-blurred edges).
double quad_mean(const Frame& f, int q) {
  const int qw = f.width / 2, qh = f.height / 2;
  const int x0 = (q % 2) * qw + qw / 4, y0 = (q / 2) * qh + qh / 4;
  double sum = 0;
  int n = 0;
  for (int y = y0; y < y0 + qh / 2; ++y)
    for (int x = x0; x < x0 + qw / 2; ++x, ++n) sum += f.view().at(x, y);
  return sum / n;
}

// Fixture: a video in a temp dir, or a skip.
class OpenCvSourceVideo : public ::testing::Test {
 protected:
  void SetUp() override {
    file = dir.path / "quad.avi";
    if (!write_video(file)) GTEST_SKIP() << "cv::VideoWriter cannot write MJPG on this machine";
  }

  // Checks the whole stream: sizes, depth, seq, the expected quadrant values and end of stream.
  // `layout[i]` is the source quadrant shown in output quadrant i; `value` gives its expected mean.
  template <class Value>
  void expect_stream(const SourceConfig& cfg, int w, int h, std::array<int, 4> layout, Value value) {
    auto s = open_opencv_source(file.string(), cfg);
    ASSERT_TRUE(s.has_value()) << (s ? "" : s.error().what);
    const FrameInfo info = (*s)->info();
    EXPECT_EQ(info.width, w);
    EXPECT_EQ(info.height, h);
    EXPECT_EQ(info.pixel_depth, 255);
    for (int i = 1; i <= kFrames; ++i) {
      auto f = (*s)->grab();
      ASSERT_TRUE(f.has_value()) << "frame " << i << ": " << (f ? "" : f.error().what);
      EXPECT_EQ(f->width, w);
      EXPECT_EQ(f->height, h);
      EXPECT_EQ(f->pixel_depth, 255);
      EXPECT_EQ(f->seq, static_cast<std::uint64_t>(i));
      for (int q = 0; q < 4; ++q)
        EXPECT_NEAR(quad_mean(*f, q), value(layout[static_cast<std::size_t>(q)]), kTol) << "frame " << i << " quadrant " << q;
    }
    auto end = (*s)->grab();
    ASSERT_FALSE(end.has_value());
    EXPECT_EQ(end.error().kind, pychron::ErrorKind::Io);
    EXPECT_EQ(end.error().what, "end of stream");
  }

  auto luma() const { return [](int q) { return kLuma[static_cast<std::size_t>(q)]; }; }
  static auto chan(int c) { return [c](int q) { return static_cast<double>(kColours[static_cast<std::size_t>(q)][static_cast<std::size_t>(c)]); }; }

  TempDir dir;
  std::filesystem::path file;
};

}  // namespace

TEST_F(OpenCvSourceVideo, LumaIdentityStream) { expect_stream({}, kW, kH, {0, 1, 2, 3}, luma()); }

TEST_F(OpenCvSourceVideo, ChannelsPickTheRightPlane) {
  // A pure-red quadrant must be bright in R and dark in B: this catches a BGR index swap.
  for (const auto [ch, idx] : {std::pair{SourceConfig::Channel::R, 0}, std::pair{SourceConfig::Channel::G, 1},
                               std::pair{SourceConfig::Channel::B, 2}}) {
    SourceConfig c;
    c.channel = ch;
    expect_stream(c, kW, kH, {0, 1, 2, 3}, chan(idx));
  }
}

TEST_F(OpenCvSourceVideo, RoiReturnsTheRequestedRectangle) {
  SourceConfig c;
  c.roi = {kW / 2, 0, kW / 2, kH};  // right half: top = TR colour, bottom = BR colour
  expect_stream(c, kW / 2, kH, {1, 1, 3, 3}, luma());
}

TEST_F(OpenCvSourceVideo, FlipsMoveQuadrants) {
  SourceConfig x;
  x.flip_x = true;  // mirror left-right
  expect_stream(x, kW, kH, {1, 0, 3, 2}, luma());
  SourceConfig y;
  y.flip_y = true;  // mirror top-bottom
  expect_stream(y, kW, kH, {2, 3, 0, 1}, luma());
}

TEST_F(OpenCvSourceVideo, RotationsAreClockwise) {
  // 90 clockwise: the old bottom-left corner becomes the top-left.
  SourceConfig r90;
  r90.rotate = 90;
  expect_stream(r90, kH, kW, {2, 0, 3, 1}, luma());
  SourceConfig r180;
  r180.rotate = 180;
  expect_stream(r180, kW, kH, {3, 2, 1, 0}, luma());
  // 270 clockwise (90 counter-clockwise): the old top-right corner becomes the top-left.
  SourceConfig r270;
  r270.rotate = 270;
  expect_stream(r270, kH, kW, {1, 3, 0, 2}, luma());
}

TEST_F(OpenCvSourceVideo, InvalidRotationIsConfigError) {
  SourceConfig c;
  c.rotate = 45;
  const auto s = open_opencv_source(file.string(), c);
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().kind, pychron::ErrorKind::Config);
}

#else

TEST(OpenCvSourceVideo, SkippedWithoutOpenCV) { GTEST_SKIP() << "built without OpenCV"; }

#endif
