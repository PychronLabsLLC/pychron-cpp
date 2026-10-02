#pragma once

// StripChartModel: the data and view settings behind the live strip chart, with
// no widgets. One TimeSeriesRing per detector, x = seconds since the first
// reading after construction or clear(). Plain C++ plus QColor/QString value
// types so it is testable without a window.
//
// A detector whose value is nullopt in a row gets a NaN-valued point at that x
// (TimeSeriesRing::range skips NaN) so the view can break the line there. Rows
// whose values are all nullopt still advance latest_x(). Readings carry values
// that are already gain-corrected; none is applied here.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QString>

#include "pychron/core/clock.hpp"
#include "pychron/core/time_series_ring.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"

namespace pychron::ui {

struct DetectorSeries {
  std::string name;
  QColor color;  // invalid: use StripChartModel::palette_color(index)
  QString units;
  QString isotope;
  bool visible = true;
};

enum class YScale { Linear, Log };

struct AxisRange {
  double lo;
  double hi;
};

class StripChartModel {
 public:
  static QColor palette_color(std::size_t index);  // 8 fixed colours, cycled

  explicit StripChartModel(std::vector<DetectorSeries> detectors);

  const std::vector<DetectorSeries>& detectors() const { return detectors_; }
  const TimeSeriesRing& series(std::size_t i) const { return rings_[i]; }

  // A timestamp earlier than the previous row's resets the chart. Detector
  // names the model does not know are ignored; a row with none is dropped.
  void append(const spectrometer::IntensityReading& r);
  void clear();
  double latest_x() const { return latest_x_; }  // 0 when empty

  // Seconds, minimum 1; the ring span follows as 1.8 * width.
  void set_scan_width(double seconds);
  double scan_width() const { return scan_width_; }

  // Hidden series keep buffering.
  void set_visible(std::size_t i, bool v);

  void set_scale(YScale s);
  YScale scale() const { return scale_; }
  void set_autoscale(bool on);
  bool autoscale() const { return autoscale_; }
  // False and unchanged unless lo < hi (and lo > 0 on Log).
  bool set_manual_y(double lo, double hi);

  AxisRange x_range() const;
  // Autoscale is recomputed at most every 0.5 s of `now`; the first call
  // always computes. Empty data keeps the previous range.
  AxisRange y_range(TimePoint now);

 private:
  AxisRange compute_autoscale() const;

  std::vector<DetectorSeries> detectors_;
  std::vector<TimeSeriesRing> rings_;
  std::map<std::string, std::size_t> index_;
  double scan_width_ = 60.0;
  YScale scale_ = YScale::Linear;
  bool autoscale_ = true;
  AxisRange manual_{0.0, 1.0};
  AxisRange auto_{0.0, 1.0};
  std::optional<TimePoint> last_auto_;
  std::optional<TimePoint> origin_;
  TimePoint last_ts_{};
  double latest_x_ = 0.0;
};

}  // namespace pychron::ui
