// The camera backend seam (live camera design, section 1).

#include <gtest/gtest.h>

#include "pychron/vision/camera_backend.hpp"

using namespace pychron;
using namespace pychron::vision;

namespace {

class Still final : public IFrameSource {
 public:
  explicit Still(int w) : w_(w) {}
  Result<Frame> grab() override { return Frame::make(w_, 2, 255, 9); }
  FrameInfo info() const override { return {w_, 2, 255, 5.0}; }

 private:
  int w_;
};

const CameraBackend* named(const std::vector<CameraBackend>& all, std::string_view name) {
  for (const auto& b : all)
    if (b.name == name) return &b;
  return nullptr;
}

}  // namespace

TEST(CameraBackend, OpenCvAndPylonAreKnown) {
  const auto all = camera_backends();
  ASSERT_NE(named(all, "opencv"), nullptr);
  ASSERT_NE(named(all, "pylon"), nullptr);
#ifdef PYCHRON_VISION_OPENCV_ENABLED
  EXPECT_TRUE(named(all, "opencv")->available);
#else
  EXPECT_FALSE(named(all, "opencv")->available);
  EXPECT_FALSE(named(all, "opencv")->unavailable_why.empty());
#endif
  EXPECT_FALSE(named(all, "pylon")->available);
  EXPECT_NE(named(all, "pylon")->unavailable_why.find("pylon"), std::string::npos);
  EXPECT_TRUE(named(all, "pylon")->list().empty());
}

TEST(CameraBackend, PylonSaysItIsNotBuilt) {
  CameraRequest request;
  request.backend = "pylon";
  const auto opened = open_camera(request);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().kind, ErrorKind::Config);
  EXPECT_NE(opened.error().what.find("built without pylon"), std::string::npos) << opened.error().what;
}

TEST(CameraBackend, AnUnknownBackendNamesTheKnownOnes) {
  CameraRequest request;
  request.backend = "webcamz";
  const auto opened = open_camera(request);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().kind, ErrorKind::Config);
  EXPECT_NE(opened.error().what.find("opencv"), std::string::npos) << opened.error().what;
  EXPECT_NE(opened.error().what.find("pylon"), std::string::npos) << opened.error().what;
}

TEST(CameraBackend, ARegisteredBackendOpensAndLists) {
  CameraBackend fake;
  fake.name = "fake";
  fake.available = true;
  fake.open = [](const CameraRequest& r, ClockFn) -> Result<std::unique_ptr<IFrameSource>> {
    if (r.device == "gone") return fail(ErrorKind::Io, "no camera " + r.device);
    return std::unique_ptr<IFrameSource>(std::make_unique<Still>(r.width));
  };
  fake.list = [] { return std::vector<CameraFound>{{"fake", "a", "the first", 640, 480, 30}}; };
  register_camera_backend(fake);
  CameraRequest request;
  request.backend = "fake";
  request.width = 7;
  auto opened = open_camera(request);
  ASSERT_TRUE(opened) << opened.error().what;
  EXPECT_EQ((*opened)->info().width, 7);
  request.device = "gone";
  EXPECT_FALSE(open_camera(request));
  ASSERT_NE(named(camera_backends(), "fake"), nullptr);
  EXPECT_EQ(named(camera_backends(), "fake")->list().size(), 1u);

  // a driver takes the place of the built-in of its name, and gives it back
  CameraBackend pylon = fake;
  pylon.name = "pylon";
  register_camera_backend(pylon);
  request.backend = "pylon";
  request.device = "a";
  EXPECT_TRUE(open_camera(request));
  unregister_camera_backend("pylon");
  EXPECT_FALSE(open_camera(request));
  unregister_camera_backend("fake");
  EXPECT_EQ(named(camera_backends(), "fake"), nullptr);
}
