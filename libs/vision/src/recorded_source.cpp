#include <utility>

#include "pychron/vision/fixture.hpp"
#include "pychron/vision/pgm.hpp"

namespace pychron::vision {

RecordedSource::RecordedSource(FixtureCase c) : case_(std::move(c)) {}

Result<Frame> RecordedSource::grab() {
  if (next_ >= case_.frames.size()) return fail(ErrorKind::Io, "end of recording");
  auto f = read_pgm(case_.dir / case_.frames[next_].file);
  if (!f) return fail(f.error());
  ++next_;
  f->seq = next_;  // 1-based, file order
  return f;
}

FrameInfo RecordedSource::info() const {
  if (case_.frames.empty()) return FrameInfo{0, 0, 255, 0.0};
  auto f = read_pgm(case_.dir / case_.frames.front().file);
  if (!f) return FrameInfo{0, 0, 255, 0.0};
  return FrameInfo{f->width, f->height, f->pixel_depth, 0.0};
}

}  // namespace pychron::vision
