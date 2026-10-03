#include <chrono>
#include <utility>

#include "pychron/vision/fixture.hpp"
#include "pychron/vision/pgm.hpp"

namespace pychron::vision {

RecordedSource::RecordedSource(FixtureCase c, ClockFn clock) : case_(std::move(c)), clock_(std::move(clock)) {
  if (!clock_) clock_ = &std::chrono::steady_clock::now;
}

Result<Frame> RecordedSource::grab() {
  if (next_ >= case_.frames.size()) return fail(ErrorKind::Io, "end of stream");
  auto f = read_pgm(case_.dir / case_.frames[next_].file);
  if (!f) return fail(f.error());
  ++next_;
  f->seq = next_;  // 1-based, file order
  f->timestamp = clock_();
  return f;
}

FrameInfo RecordedSource::info() const {
  if (case_.frames.empty()) return FrameInfo{0, 0, 255, 0.0};
  auto f = read_pgm(case_.dir / case_.frames.front().file);
  if (!f) return FrameInfo{0, 0, 255, 0.0};
  return FrameInfo{f->width, f->height, f->pixel_depth, 0.0};
}

}  // namespace pychron::vision
