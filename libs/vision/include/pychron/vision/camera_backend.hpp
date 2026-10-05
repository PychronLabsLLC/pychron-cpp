#pragma once

// Where live cameras come from (live camera design, section 1). A backend is
// one way of reaching cameras: OpenCV (whatever it can open: a built-in or
// USB camera, a video file) and, when its driver is built, Basler's pylon
// (GigE and USB3 Vision). A backend opens a request into a frame source and
// lists the cameras it can find.
//
// A source a backend returns may block in grab() for as long as its camera
// does: put it behind a LiveFeed (live_feed.hpp).

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/vision/opencv_source.hpp"
#include "pychron/vision/source.hpp"

namespace pychron::vision {

struct CameraRequest {
  std::string backend;  // "opencv", "pylon", or one registered
  std::string device;   // the backend's name for the camera: an index, a file, a serial number
  int width = 0;        // 0: the camera's own
  int height = 0;
  double fps = 0;
  SourceConfig shape;                          // roi, channel, flips, rotation
  std::map<std::string, std::string> options;  // the backend's own (exposure_us, gain_db, ...)
};

struct CameraFound {
  std::string backend;
  std::string device;       // what a request names it by
  std::string description;  // for a person
  int width = 0;
  int height = 0;
  double fps = 0;
};

struct CameraBackend {
  std::string name;
  bool available = false;       // this build can open its cameras
  std::string unavailable_why;  // when it cannot
  std::function<Result<std::unique_ptr<IFrameSource>>(const CameraRequest&, ClockFn)> open;
  // Looking for cameras may open each one in turn: not for a hot path.
  std::function<std::vector<CameraFound>()> list;
};

// "opencv" and "pylon" (available or not), then whatever was registered.
std::vector<CameraBackend> camera_backends();
// Adds a backend, or takes the place of the one of its name: how a driver
// built elsewhere, or a test's fake, comes in.
void register_camera_backend(CameraBackend backend);
// Forgets a registered backend; a built-in of that name is itself again.
void unregister_camera_backend(std::string_view name);

// Config error for an unknown backend (naming the known ones) and for one
// this build does not have; otherwise whatever the backend answers. An empty
// `clock` is steady_clock.
Result<std::unique_ptr<IFrameSource>> open_camera(const CameraRequest& request, ClockFn clock = {});

}  // namespace pychron::vision
