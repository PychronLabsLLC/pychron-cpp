#include "pychron/vision/legacy_finder.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

#ifdef PYCHRON_VISION_OPENCV_ENABLED
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp>  // OpenCV 5: contour geometry left imgproc
#endif
#endif

namespace pychron::vision {

#ifdef PYCHRON_VISION_OPENCV_ENABLED

namespace {

// Constants of the original pipeline (pychron/mv/locator.py, lumen_detector.py).
constexpr double kGamma = 2.0;
constexpr double kUnsharpRadius = 10.0;
constexpr double kUnsharpAmount = 3.0;
constexpr double kSmoothFactor = 0.001;  // approxPolyDP epsilon / arc length
constexpr double kMinArea = 100.0;       // px^2
constexpr int kMinVertices = 4;          // "more than 3"
constexpr std::size_t kMinContourPoints = 5;
constexpr std::size_t kMaxTargets = 15;
constexpr double kMinWhite = 0.25, kMaxWhite = 0.75;  // threshold limiting (Hole)
constexpr double kConvexityMin = 0.35;
constexpr double kLegacyPi = 3.1415;  // the original uses 3.1415, not pi

// One accepted contour, with the quantities the legacy Target carried.
struct Cand {
  std::vector<cv::Point> contour;
  double area = 0;              // polygon area after approxPolyDP
  double min_enclose_area = 0;  // area of the minimum enclosing circle of the polygon
  cv::Point centroid;           // truncated to integers, as the original does
  double perimeter = 0;         // arc length of the raw contour
};

// frame / depth, gamma 2, unsharp mask, rescale to 0..255 (locator._preprocess).
cv::Mat preprocess(const FrameView& v) {
  const double depth = v.pixel_depth > 0 ? v.pixel_depth : 255.0;
  cv::Mat f(v.height, v.width, CV_64F);
  for (int y = 0; y < v.height; ++y) {
    auto* row = f.ptr<double>(y);
    for (int x = 0; x < v.width; ++x) row[x] = v.at(x, y) / depth;
  }
  cv::pow(f, kGamma, f);

  // skimage unsharp_mask: image + (image - gaussian(image, radius)) * amount, clipped to [0, 1].
  // Kernel radius 40 is skimage's truncate = 4 sigma; reflect matches its default border.
  cv::Mat blurred;
  cv::GaussianBlur(f, blurred, cv::Size(81, 81), kUnsharpRadius, kUnsharpRadius, cv::BORDER_REFLECT);
  cv::Mat sharp;
  cv::addWeighted(f, 1.0 + kUnsharpAmount, blurred, -kUnsharpAmount, 0.0, sharp);
  cv::min(sharp, 1.0, sharp);
  cv::max(sharp, 0.0, sharp);

  // rescale_intensity. A uniform image has no range to stretch: all zero (the original divides by zero).
  double lo = 0, hi = 0;
  cv::minMaxLoc(sharp, &lo, &hi);
  if (hi - lo <= 0) return cv::Mat::zeros(v.height, v.width, CV_64F);
  return (sharp - lo) * (255.0 / (hi - lo));
}

// Inside the disk (strict, like skimage.draw.disk) keep the value; outside write `outside`.
// The original centers the disk at (w/2, h/2) in index space, not on the pixel center.
void apply_mask(cv::Mat& src, double radius, double outside) {
  if (radius <= 0) return;
  const double cx = src.cols / 2.0, cy = src.rows / 2.0;
  for (int y = 0; y < src.rows; ++y) {
    auto* row = src.ptr<double>(y);
    for (int x = 0; x < src.cols; ++x) {
      const double dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy >= radius * radius) row[x] = outside;
    }
  }
}

// One threshold of the sweep: foreground is src < t, holes filled, then contour -> polygon candidates.
// Returns nothing when threshold limiting rejects the threshold.
std::vector<Cand> find_at(const cv::Mat& src, double t, bool limit) {
  cv::Mat fg(src.rows, src.cols, CV_8U);
  for (int y = 0; y < src.rows; ++y) {
    const auto* s = src.ptr<double>(y);
    auto* d = fg.ptr<std::uint8_t>(y);
    for (int x = 0; x < src.cols; ++x) d[x] = s[x] >= t ? 0 : 255;
  }

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(fg, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

  if (limit) {
    // The original fills holes first and takes the white fraction of the filled image.
    cv::Mat filled = cv::Mat::zeros(fg.size(), CV_8U);
    cv::drawContours(filled, contours, -1, cv::Scalar(255), cv::FILLED);
    const double white = static_cast<double>(cv::countNonZero(filled)) / static_cast<double>(filled.total());
    if (white > kMaxWhite || white < kMinWhite) return {};
  }

  std::vector<Cand> out;
  for (auto& c : contours) {
    const double perimeter = cv::arcLength(c, true);
    std::vector<cv::Point> poly;
    cv::approxPolyDP(c, poly, perimeter * kSmoothFactor, true);
    const double area = std::abs(cv::contourArea(poly));
    const cv::Moments m = cv::moments(c);
    if (m.m00 == 0) continue;
    if (static_cast<int>(poly.size()) < kMinVertices) continue;
    if (!(area > kMinArea)) continue;
    if (c.size() < kMinContourPoints) continue;

    cv::Point2f ctr;
    float r = 0;
    cv::minEnclosingCircle(poly, ctr, r);
    Cand k;
    k.area = area;
    k.min_enclose_area = static_cast<double>(r) * static_cast<double>(r) * kLegacyPi;
    k.centroid = cv::Point(static_cast<int>(m.m10 / m.m00), static_cast<int>(m.m01 / m.m00));
    k.perimeter = perimeter;
    k.contour = std::move(c);
    out.push_back(std::move(k));
  }
  return out;
}

// locator._low_search: rising threshold from mean/2, +2 on a hit, a growing step on a miss.
template <class Find>
std::vector<Cand> low_search(const cv::Mat& src, Find&& find, double* first_threshold) {
  std::vector<Cand> ts;
  int t = static_cast<int>(cv::mean(src)[0] / 2.0);
  int step = 0;
  while (t <= 254) {
    auto tt = find(t);
    if (!tt.empty()) {
      if (first_threshold && ts.empty()) *first_threshold = t;
      t += 2;
      step = std::max(1, step - 1);
      for (auto& c : tt) ts.push_back(std::move(c));
    } else {
      ++step;
      t += step;
    }
    if (ts.size() >= kMaxTargets) break;
  }
  return ts;
}

// lumen_detector.choose_target's saturation on the raw frame: pixels at or below half
// the frame maximum are zeroed, then sum / ((area + perimeter / 2) * depth).
double legacy_saturation(const FrameView& v, std::uint16_t cutoff, const Cand& c, double depth) {
  const cv::Rect br = cv::boundingRect(c.contour);
  cv::Mat mask = cv::Mat::zeros(br.size(), CV_8U);
  const std::vector<std::vector<cv::Point>> one{c.contour};
  cv::drawContours(mask, one, 0, cv::Scalar(255), cv::FILLED, cv::LINE_8, cv::noArray(), INT_MAX,
                   cv::Point(-br.x, -br.y));
  double sum = 0;
  for (int y = 0; y < br.height; ++y)
    for (int x = 0; x < br.width; ++x) {
      if (!mask.at<std::uint8_t>(y, x)) continue;
      const int px = br.x + x, py = br.y + y;
      if (px < 0 || py < 0 || px >= v.width || py >= v.height) continue;
      const std::uint16_t p = v.at(px, py);
      if (p > cutoff) sum += p;
    }
  const double denom = (c.area + c.perimeter / 2.0) * depth;
  return denom > 0 ? sum / denom : 0.0;
}

Target to_target(const Cand& c, double score) {
  Target t;
  t.center_px = {static_cast<double>(c.centroid.x), static_cast<double>(c.centroid.y)};
  t.area_px = c.area;
  t.radius_px = std::sqrt(c.area / std::numbers::pi);
  t.circularity = c.perimeter > 0 ? std::min(1.0, 4 * std::numbers::pi * c.area / (c.perimeter * c.perimeter)) : 0.0;
  t.score = score;
  return t;
}

class LegacyFinder final : public ITargetFinder {
 public:
  std::vector<Target> find(const FrameView& v, const FinderParams& p, FinderDebug* debug) override {
    if (debug) {
      *debug = FinderDebug{};
      debug->width = std::max(v.width, 0);
      debug->height = std::max(v.height, 0);
    }
    if (v.width <= 0 || v.height <= 0 || v.data == nullptr) return {};

    const bool glow = p.mode == FinderMode::Glow;
    cv::Mat src = preprocess(v);
    // Glow zeroes outside the mask, then inverts. Hole fills outside with 255 (the original's
    // annular mask) and is not inverted. Either way the outside never becomes foreground.
    apply_mask(src, p.mask_radius_px, glow ? 0.0 : 255.0);
    if (glow) src = 255.0 - src;  // the original's numpy.invert on a float image raised; this is the intent

    // dim for the Hole area window is the expected radius.
    const double dim = p.expected_radius_px;
    // Hole needs a radius for its area window; with none the window is empty and every
    // candidate would be rejected, so say so now rather than sweep for nothing.
    if (!glow && !(dim > 0)) return {};
    const double a_min = (0.5 * dim) * (0.5 * dim) * kLegacyPi;
    const double a_max = (1.25 * dim) * (1.25 * dim) * kLegacyPi;
    // The original gates on 0.75 * px_per_mm px, which FinderParams does not carry. A hole or
    // glow is about 1 mm across, so 0.75 * the expected diameter stands in; no radius, no gate.
    const double gate = 0.75 * 2.0 * dim;
    const double cx = v.width / 2.0, cy = v.height / 2.0;
    auto center_dist = [&](const Cand& c) { return std::hypot(c.centroid.x - cx, c.centroid.y - cy); };
    auto near_center = [&](const Cand& c) { return gate <= 0 || center_dist(c) < gate; };

    double first_t = 0;
    std::vector<Cand> found = low_search(
        src, [&](int t) { return find_at(src, t, !glow); }, &first_t);
    if (debug) {
      debug->threshold = static_cast<std::uint16_t>(first_t);
      debug->components = static_cast<int>(found.size());
    }

    std::vector<Target> out;
    if (glow) {
      std::erase_if(found, [&](const Cand& c) { return !near_center(c); });
      if (found.empty()) return out;
      std::stable_sort(found.begin(), found.end(), [](const Cand& a, const Cand& b) { return a.area < b.area; });  // Python's sorted() is stable
      std::uint16_t peak = 0;
      for (int y = 0; y < v.height; ++y)
        for (int x = 0; x < v.width; ++x) peak = std::max(peak, v.at(x, y));
      const auto cutoff = static_cast<std::uint16_t>(peak * 0.5);
      const double depth = v.pixel_depth > 0 ? v.pixel_depth : 255.0;
      std::vector<std::pair<Cand*, double>> scored;
      for (auto& c : found) scored.emplace_back(&c, legacy_saturation(v, cutoff, c, depth));
      // choose_target: highest saturation first; all equal means smallest area (already sorted).
      const bool all_equal = std::all_of(scored.begin(), scored.end(), [&](const auto& s) { return s.second == scored.front().second; });
      if (!all_equal)
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      for (const auto& [c, s] : scored) out.push_back(to_target(*c, s));
      return out;
    }

    // Hole: _filter_test (convexity, center, area window); best is nearest the center.
    std::vector<std::pair<double, Target>> kept;
    for (const auto& c : found) {
      const double convexity = c.min_enclose_area > 0 ? c.area / c.min_enclose_area : 0.0;
      if (!(convexity > kConvexityMin)) continue;
      if (!near_center(c)) continue;
      if (!(a_max > c.area && c.area > a_min)) continue;
      kept.emplace_back(center_dist(c), to_target(c, convexity));  // the legacy test statistic
    }
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& k : kept) out.push_back(std::move(k.second));
    return out;
  }
};

}  // namespace

bool opencv_enabled() { return true; }

std::unique_ptr<ITargetFinder> make_legacy_finder() { return std::make_unique<LegacyFinder>(); }

#else

bool opencv_enabled() { return false; }

std::unique_ptr<ITargetFinder> make_legacy_finder() { return nullptr; }

#endif

}  // namespace pychron::vision
