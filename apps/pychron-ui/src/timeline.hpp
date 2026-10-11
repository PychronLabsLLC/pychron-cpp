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
// The view shows a window of the queue's time, not all of it: a queue of a
// thousand runs drawn whole is a pixel a run. It follows the last few runs
// until the operator moves the window, and can show the whole queue.
//
// Times are the line's clock (simulated time under --sim-speed), in seconds
// since the first event. Open segments grow with advance(now).
//
// The model keeps every segment of the queue. Neither an event nor a paint
// walks them all: open segments are indexed, and visible() gives the ones a
// time window shows (docs/superpowers/specs/2026-10-11-timeline-window-design.md).

#include <cstddef>

#include <map>
#include <optional>
#include <string>
#include <utility>
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

  // Indices, ascending, of the segments that show in [t0, t1].
  [[nodiscard]] std::vector<std::size_t> visible(double t0, double t1) const;
  // When each run started, in the order they did.
  const std::vector<double>& run_starts() const noexcept { return run_starts_; }
  std::size_t open_count() const noexcept { return open_.size(); }

 private:
  double seconds(TimePoint ts);
  void close_run(const std::string& run_id, double at);  // its open state and waits
  void add(TimelineSegment s);                           // open, and indexed as such
  // Closes the open segments `which` picks, each at the time it gives.
  template <typename Pick>
  void close_open(Pick which);

  std::optional<TimePoint> origin_;
  double now_ = 0;
  std::vector<TimelineSegment> segments_;
  std::map<std::string, int> lanes_;        // run id -> lane
  std::map<std::string, QString> names_;    // run id -> identifier
  std::vector<bool> busy_;                  // run lane i + 1 in use
  std::vector<std::size_t> open_;           // segments without an end, ascending
  std::vector<double> run_starts_;
  double longest_ = 0;                      // the longest closed segment
  bool ordered_ = true;                     // starts never went backwards
};

class TimelineView : public QWidget {
  Q_OBJECT

 public:
  // Follow: from the start of the follow_runs()-th most recent run to now.
  // Free: where the operator put it. WholeQueue: the first event to now.
  enum class Mode { Follow, Free, WholeQueue };
  static constexpr int kFollowRuns = 6;
  static constexpr double kMinSpan = 10;  // seconds; a window is never narrower

  explicit TimelineView(const TimelineModel& model, QWidget* parent = nullptr);
  Mode mode() const noexcept { return mode_; }
  void set_mode(Mode mode);
  int follow_runs() const noexcept { return follow_runs_; }
  void set_follow_runs(int runs);
  // The time shown, in the model's seconds.
  std::pair<double, double> time_window() const;
  // Shows [t0, t1], kept inside the queue's time and no narrower than
  // kMinSpan; the view is then Free.
  void set_time_window(double t0, double t1);
  // How many segments the last paint drew.
  int drawn_count() const noexcept { return drawn_; }
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
  double full_end() const;  // the end of the whole queue's window

  const TimelineModel& model_;
  Mode mode_ = Mode::Follow;
  int follow_runs_ = kFollowRuns;
  double t0_ = 0, t1_ = kMinSpan;  // the Free window
  int drawn_ = 0;
};

}  // namespace pychron::ui
