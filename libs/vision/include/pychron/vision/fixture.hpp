#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/source.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

// What a case may be used for: Synthetic and ScreenRecording frames get loose
// tolerances and never tune constants; Raw camera frames get tight ones.
enum class Provenance { Synthetic, ScreenRecording, Raw };

struct FixtureFrame {
  std::string file;                  // relative to the case directory
  std::optional<Vec2> center_px;     // marked target centre, in this frame's pixels
  bool skip = false;                 // known failure: fixture tests ignore the frame
};

struct FixtureCase {
  std::filesystem::path dir;
  Provenance provenance = Provenance::Synthetic;
  FinderMode mode = FinderMode::Hole;
  double expected_radius_px = 0;
  double tolerance_px = 4;
  std::string channel = "luma";  // informational: luma | r | g | b
  std::string note;              // informational
  std::vector<FixtureFrame> frames;
};

// Reads dir/case.toml and checks every listed frame file exists.
Result<FixtureCase> load_case(const std::filesystem::path& dir);
// Writes dir/case.toml (the frame files are not touched).
Result<void> save_case(const FixtureCase&);

// Replays the frames of a case in file order, seq from 1, then fails with Io
// ("end of stream"). Every frame is stamped from `clock` at grab() time
// (default steady_clock), because recorded frames carry no time of their own.
class RecordedSource final : public IFrameSource {
 public:
  using ClockFn = std::function<TimePoint()>;
  explicit RecordedSource(FixtureCase c, ClockFn clock = {});

  Result<Frame> grab() override;
  FrameInfo info() const override;

 private:
  FixtureCase case_;
  ClockFn clock_;
  std::size_t next_ = 0;
};

class FrameRecorder {
 public:
  FrameRecorder(std::filesystem::path dir, Provenance, FinderMode, double expected_radius_px);

  // Writes NNNN.pgm (numbered from 0001) into the directory.
  Result<void> add(const FrameView&, std::optional<Vec2> center_px = {});
  // Writes case.toml.
  Result<void> finish();

 private:
  FixtureCase case_;
};

}  // namespace pychron::vision
