// A real camera, when there is one to try: not run unless
// PYCHRON_TEST_CAMERA names its index (no CI machine has a camera, and on
// macOS the first open asks a person).
//
//   PYCHRON_TEST_CAMERA=0 ctest --test-dir build/dev -R RealCamera

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "pychron/vision/camera_backend.hpp"
#include "pychron/vision/live_feed.hpp"

using namespace pychron;
using namespace pychron::vision;

TEST(RealCamera, GivesFramesThroughALiveFeed) {
  const char* index = std::getenv("PYCHRON_TEST_CAMERA");
  if (index == nullptr || *index == '\0') GTEST_SKIP() << "set PYCHRON_TEST_CAMERA=<index> to open a real camera";
  CameraRequest request;
  request.backend = "opencv";
  request.device = index;
  LiveFeedOptions options;
  options.timeout = std::chrono::milliseconds(5000);  // a camera takes a moment to wake
  LiveFeed feed([request](ClockFn stamp) { return open_camera(request, std::move(stamp)); }, options);
  const auto opened = feed.wait_open();
  ASSERT_TRUE(opened) << opened.error().what;
  std::uint64_t last = 0;
  for (int i = 0; i < 5; ++i) {
    const auto frame = feed.grab();
    ASSERT_TRUE(frame) << frame.error().what;
    EXPECT_GT(frame->width, 0);
    EXPECT_GT(frame->height, 0);
    EXPECT_EQ(frame->data.size(), static_cast<std::size_t>(frame->width) * static_cast<std::size_t>(frame->height));
    EXPECT_GT(frame->seq, last);
    last = frame->seq;
  }
  EXPECT_GT(feed.latest().fps, 0);
}
