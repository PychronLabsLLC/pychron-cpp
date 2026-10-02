#pragma once

// The queue's phase timeline (experiment-window design 5.3): what each run
// did when, and what the executor waited for.
//
//   Waits   the queue's delays and the runs' resource waits (extraction
//           device, minimum pump time), grey, labelled with the reason
//   Lane n  one run at a time: a segment per run state (preparing,
//           extracting, ... saving) in the queue table's state colours.
//           An overlapped run starts while the previous one still measures,
//           so it takes the next free lane: overlap shows as two lanes.
//
// Times are the line's clock (simulated time under --sim-speed), in seconds
// since the first event. Open segments grow with advance(now).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QString>
#include <QTimer>
#include <QWidget>

#include "pychron/core/clock.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/run/state.hpp"

namespace pychron::ui {

struct TimelineSegment {
  int lane = 0;  // 0: waits; 1..: run lanes
  std::string run_id;
  QString label;                                     // identifier, or "<who>: <reason>" for a wait
  std::optional<experiment::run::RunState> state;    // nullopt for a wait
  double start = 0;                                  // seconds since the first event
  std::optional<double> end;                         // nullopt while open
  std::optional<double> planned_end;                 // a timed wait's end
};

class TimelineModel {
 public:
  void clear();
  void on_run_started(const experiment::executor::RunStarted& e);
  void on_state(const experiment::run::RunStateChanged& e);
  void on_waiting(const experiment::executor::ExecutorWaiting& e);
  void on_queue_ended(TimePoint ts);  // closes whatever is open
  void advance(TimePoint now);        // open segments extend to `now`

  int run_lanes() const noexcept { return static_cast<int>(busy_.size()); }
  const std::vector<TimelineSegment>& segments() const noexcept { return segments_; }
  double now() const noexcept { return now_; }  // seconds since the first event
  // Where a segment ends on screen: its end, else the planned end or now.
  double drawn_end(const TimelineSegment& s) const;
  std::optional<int> lane_of(const std::string& run_id) const;

 private:
  double seconds(TimePoint ts);
  void close_run(const std::string& run_id, double at);  // its open state and waits

  std::optional<TimePoint> origin_;
  double now_ = 0;
  std::vector<TimelineSegment> segments_;
  std::map<std::string, int> lanes_;        // run id -> lane
  std::map<std::string, QString> names_;    // run id -> identifier
  std::vector<bool> busy_;                  // run lane i + 1 in use
};

class TimelineView : public QWidget {
  Q_OBJECT

 public:
  explicit TimelineView(const TimelineModel& model, QWidget* parent = nullptr);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  // The segment under a point, for tooltips and tests.
  const TimelineSegment* segment_at(QPoint p) const;
  QRectF segment_rect(const TimelineSegment& s) const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool event(QEvent* event) override;

 private:
  QRectF plot_area() const;
  double span() const;

  const TimelineModel& model_;
};

}  // namespace pychron::ui
