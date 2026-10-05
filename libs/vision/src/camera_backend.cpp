#include "pychron/vision/camera_backend.hpp"

#include <algorithm>
#include <mutex>

namespace pychron::vision {

namespace {

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}

std::vector<CameraBackend>& registered() {
  static std::vector<CameraBackend> all;
  return all;
}

CameraBackend opencv_backend() {
  CameraBackend b;
  b.name = "opencv";
  b.available = opencv_built();
  if (!b.available) b.unavailable_why = "built without OpenCV (-DPYCHRON_VISION_OPENCV=ON)";
  b.open = [](const CameraRequest& request, ClockFn clock) { return open_opencv_camera(request, std::move(clock)); };
  b.list = [] { return list_opencv_cameras(); };
  return b;
}

// The driver is not written yet: its place is kept, and says so.
CameraBackend pylon_backend() {
  CameraBackend b;
  b.name = "pylon";
  b.available = false;
  b.unavailable_why = "built without pylon: the Basler pylon driver is not part of this build";
  b.open = [why = b.unavailable_why](const CameraRequest&, ClockFn) -> Result<std::unique_ptr<IFrameSource>> {
    return fail(ErrorKind::Config, why);
  };
  b.list = [] { return std::vector<CameraFound>{}; };
  return b;
}

}  // namespace

std::vector<CameraBackend> camera_backends() {
  std::vector<CameraBackend> all{opencv_backend(), pylon_backend()};
  std::lock_guard lock(registry_mutex());
  for (const auto& extra : registered()) {
    const auto same = std::find_if(all.begin(), all.end(), [&](const CameraBackend& b) { return b.name == extra.name; });
    if (same != all.end()) *same = extra;
    else all.push_back(extra);
  }
  return all;
}

void register_camera_backend(CameraBackend backend) {
  std::lock_guard lock(registry_mutex());
  auto& all = registered();
  std::erase_if(all, [&](const CameraBackend& b) { return b.name == backend.name; });
  all.push_back(std::move(backend));
}

void unregister_camera_backend(std::string_view name) {
  std::lock_guard lock(registry_mutex());
  std::erase_if(registered(), [&](const CameraBackend& b) { return b.name == name; });
}

Result<std::unique_ptr<IFrameSource>> open_camera(const CameraRequest& request, ClockFn clock) {
  const auto all = camera_backends();
  for (const auto& backend : all) {
    if (backend.name != request.backend) continue;
    if (!backend.available) return fail(ErrorKind::Config, "camera backend " + backend.name + ": " + backend.unavailable_why);
    if (!backend.open) return fail(ErrorKind::Config, "camera backend " + backend.name + " cannot open cameras");
    return backend.open(request, std::move(clock));
  }
  std::string known;
  for (const auto& backend : all) known += (known.empty() ? "" : ", ") + backend.name;
  return fail(ErrorKind::Config, "unknown camera backend '" + request.backend + "' (there is: " + known + ")");
}

}  // namespace pychron::vision
