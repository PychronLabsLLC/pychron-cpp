#pragma once

#include <cstdint>
#include <vector>

#include "pychron/vision/frame.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

struct Target {
  Point2 center_px;
  double radius_px = 0;
  double area_px = 0;
  double circularity = 0;
  double score = 0;  // Hole: circularity x radius match. Glow: saturation.
};

enum class FinderMode { Hole, Glow };  // dark on light, bright on dark

struct FinderParams {
  FinderMode mode = FinderMode::Hole;
  double expected_radius_px = 0;
  double radius_tol = 0.35;
  double mask_radius_px = 0;  // <= 0: no mask. Disk centred on the frame centre.
  double glow_fraction = 0.5;
};

// Feeds UI overlays and failure snapshots; costs nothing when not requested.
struct FinderDebug {
  std::uint16_t threshold = 0;
  int components = 0;
  int rejected_radius = 0, rejected_circularity = 0, rejected_edge = 0, rejected_area = 0;
  std::vector<std::uint8_t> mask;  // thresholded foreground, row-major width*height
  int width = 0, height = 0;
};

class ITargetFinder {
 public:
  virtual ~ITargetFinder() = default;
  virtual std::vector<Target> find(const FrameView&, const FinderParams&, FinderDebug* debug = nullptr) = 0;
};

// Works on the whole view it is given; cropping to a region of interest is the caller's job.
class SimpleFinder final : public ITargetFinder {
 public:
  std::vector<Target> find(const FrameView&, const FinderParams&, FinderDebug* debug = nullptr) override;
};

// Glow targets: the same value as Target::score, in [0, 1].
double saturation(const Target&);

}  // namespace pychron::vision
