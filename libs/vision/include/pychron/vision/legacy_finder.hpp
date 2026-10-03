#pragma once

#include <memory>

#include "pychron/vision/finder.hpp"

namespace pychron::vision {

// True when the library was built with OpenCV (PYCHRON_VISION_OPENCV).
bool opencv_enabled();

// A port of the original Python target-finding pipeline, kept so it can be
// compared with SimpleFinder on the same frames. nullptr when built without OpenCV.
//
// FinderParams::mode picks the legacy path: Glow is the dragonfly path
// (inverted sweep, the centre-distance gate, ranked by choose_target, score =
// legacy saturation); Hole is the autocenter path (threshold limiting and the
// _filter_test rules, nearest the frame centre first). Hole needs the target to
// fill 25 to 75 percent of the view, so with Autocenter's default crop it
// returns no target.
std::unique_ptr<ITargetFinder> make_legacy_finder();

}  // namespace pychron::vision
