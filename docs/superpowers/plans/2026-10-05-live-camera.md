# Live Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A laser's camera can be a real one (OpenCV now, pylon's seam and config ready), shown live, never able to hold up a stop, with pixel scale measured from jogs, snapshots, and a way to find cameras.

**Architecture:** `libs/vision` gains a backend seam, a `LiveFeed` that owns the blocking read on its own thread, and a PNG writer. `libs/laser` learns the new sources, `use = "view"`, a measured scale, and snapshots. `Lasers`, `elctl laser` and the laser window are wired to them.

**Tech Stack:** C++20, optional OpenCV (already optional), Qt6 only in the UI. No new dependency.

**Spec:** `docs/superpowers/specs/2026-10-05-live-camera-design.md`

## Global Constraints

- OpenCV headers stay in `legacy_finder.cpp` and `opencv_source.cpp` only. Qt stays out of `libs/`.
- No test in CI opens a real camera (opt-in `PYCHRON_TEST_CAMERA`).
- `LiveFeed::grab()` never waits longer than its timeout; `latest()` never waits.
- A `view` camera never causes a stage move.
- TDD: each test is seen to fail first. Never skip a failing test.
- Never commit `configs/examples/nmgrl/conditionals/`.

## Review Focus

- A camera whose read never returns: `grab()` times out, the feed's destructor still ends (the thread cannot be joined while the read hangs).
- The camera comes back after being lost: frames flow again without restarting the program.
- A frame read before the stage settled is handed to autocenter as fresh.
- A measured scale with the wrong sign: autocenter runs away from the hole.
- A snapshot name that climbs out of the snapshots directory.

---

### Task 1: PNG writer (`libs/vision`)
`png.hpp`: `Result<void> write_png(const std::filesystem::path&, const FrameView&)` — grey, 8 bit (depth <= 255) or 16 bit. Tests `tests/vision/test_png.cpp`: signature and IHDR; every chunk's CRC; stored-deflate payload decodes back to the pixels (8 and 16 bit, a sub-rectangle view with stride > width, a 70 000-byte image that needs more than one stored block); unwritable path is an Io error.

### Task 2: Backend seam (`libs/vision`)
`camera_backend.hpp`:
```cpp
struct CameraRequest { std::string backend, device; int width = 0, height = 0; double fps = 0; SourceConfig shape; std::map<std::string, std::string> options; };
struct CameraFound { std::string backend, device, description; int width = 0, height = 0; double fps = 0; };
struct CameraBackend { std::string name; bool available = false; std::string unavailable_why;
  std::function<Result<std::unique_ptr<IFrameSource>>(const CameraRequest&, ClockFn)> open;
  std::function<std::vector<CameraFound>()> list; };
std::vector<CameraBackend> camera_backends();                       // opencv, pylon; then registered ones
void register_camera_backend(CameraBackend backend);               // replaces one of the same name (tests, a driver)
void unregister_camera_backend(std::string_view name);             // back to the built-in, if any
Result<std::unique_ptr<IFrameSource>> open_camera(const CameraRequest&, ClockFn clock = {});
```
`open_opencv_source` gains optional width/height/fps. Tests: unknown backend names the known ones; pylon says built without; a registered fake opens and lists; opencv availability matches the build.

### Task 3: `LiveFeed` (`libs/vision`)
```cpp
struct LiveFeedOptions { std::chrono::milliseconds timeout{1000}; std::chrono::milliseconds reopen{1000}; };
class LiveFeed final : public IFrameSource {
 public:
  using Opener = std::function<Result<std::unique_ptr<IFrameSource>>()>;
  explicit LiveFeed(Opener open, LiveFeedOptions options = {});
  Result<void> wait_open();          // the first open's result, within timeout
  Result<Frame> grab() override; FrameInfo info() const override;
  struct Latest { std::optional<Frame> frame; std::chrono::milliseconds age{0}; double fps = 0; std::string error; };
  Latest latest() const;
};
```
The reader thread and its state are shared-owned, so a destructor does not wait for a read that never returns (the thread is detached then, and owns what it uses). Tests (`test_live_feed.cpp`, fake sources with a gate): frame newer than the call; timeout within bounds on a hung read; fail-fast after; error passed through; reopen after loss; destructor returns while a read hangs; `latest()` age and error; `wait_open` failure.

### Task 4: Camera config and the use rule (`libs/laser`)
`CameraSource::{OpenCv, Pylon}`, `CameraUse::{Center, View}`, the two tables, `bool live() const`, `vision::CameraRequest request() const`, `std::optional<vision::CameraStageMap> measured` used by `map()` and `scale_px_per_mm()`. `usable_for_autocenter`: live over a simulated stage refused; `View` refused ("for looking only"). `make_frame_source` builds a `LiveFeed` for live sources. `LaserSystem::attach_viewer(config, frames)`; `can_center()`; `view()` through `LiveFeed::latest()`; `CameraView::{age_ms, fps}`. `check_lab_queue`: a view camera is no camera for a dragonfly. `Lasers`: view cameras attached as viewers; `notes()` for what does not block a queue. Tests in `tests/laser/test_camera.cpp`, `test_laser_watch.cpp`, `tests/experiment/test_lab_extraction.cpp`, `tests/integration/test_lab_session.cpp`.

### Task 5: Scale from jogs (`libs/laser`)
`camera_scale.hpp`: `struct ScaleMeasurement { vision::CameraStageMap map; double px_per_mm; bool flip_x, flip_y; double skew_deg, anisotropy; }`; `Result<ScaleMeasurement> scale_from(std::span<const vision::JogPair>)` (the 3% and 3 degree rules); `class CameraScaleStore` (`<lab>/camera_scales/<device>.toml`: load, save, clear); `Result<ScaleMeasurement> LaserSystem::measure_scale(double step_mm, const std::function<Result<void>()>& arrive)`; `void LaserSystem::set_measured_scale(const ScaleMeasurement&)`. `Lab::camera_scales`; `Lasers` applies a stored scale. Tests: the simulated camera's scale and flips recovered (both flip settings); nothing in view refused; a mirrored fake refused as skew or told as a flip; store round trip; autocenter converges using a measured map.

### Task 6: Snapshots (`libs/laser`)
`LaserSystem` is an `IImaging` when it has a camera and the driver has none: `set_snapshot_dir(path)`, `snapshot(name)` per spec section 5; recording is not supported. `Lasers` sets `<lab>/snapshots/<device>`. Tests: file written and readable; time for a name; never over a file; a name with a path refused; no camera is not_supported.

### Task 7: `elctl laser cameras | camera-scale | snapshot`
Tests in `apps/elctl/tests/test_laser_commands.cpp` on the simulator and a registered fake backend.

### Task 8: Laser window
`LaserBridge::snapshot_to_file()`, `measure_scale(double step_mm)` (signal `scaleMeasured`), camera age/lost shown, buttons `snapshot` and `measure_scale`. Bundle `NSCameraUsageDescription`. Example `cameras.toml` documents the new keys; `docs/dev_setup.md`; spec "As built". An opt-in real-camera test.
