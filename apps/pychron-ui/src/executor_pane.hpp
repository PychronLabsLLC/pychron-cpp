#pragma once

// ExecutorPane (experiment-window design 5.3): what the executor is doing and
// the controls to start and stop it.
//
//   state, runs done n/N, progress over the runnable rows
//   current run: identifier, run state, block, counts bar
//   the executor's current wait (reason and duration)
//   Start / Stop / Cancel / Abort / Truncate / Resume / Skip Held Run
//   conditionals that tripped; a table of events, each with the time it was
//   seen on the line's clock: one row per run start/finish, thing a run
//   said (a script's info(), whether a hole was centered), queue edit,
//   peak-center result and queue end; records left in the spool
//   what the session cannot do (LabSession::problems), from the start
//
// Start is the window's to carry out (it knows the queue and the selected
// row): the pane emits startRequested. The other controls go straight to the
// bridge. Cancel, Abort and Skip Held Run ask first (default No).
//
// While the queue holds (spec 4.6) the events say why, for which links and
// until when (the hold limit, on the line's clock), and Resume and Skip Held
// Run are enabled: Resume tries the held row now, without waiting for its
// links to settle; Skip Held Run drops the held row, only when it has not
// started (a run that failed past its point of no return is not held for).

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include "experiment_bridge.hpp"
#include "timeline.hpp"

class QFrame;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class ExecutorPane : public QWidget {
  Q_OBJECT

 public:
  using Confirm = std::function<bool(const QString& title, const QString& question)>;

  explicit ExecutorPane(ExperimentBridge& bridge, QWidget* parent = nullptr);

  // From the window: whether the queue checks (Start needs it while idle) and
  // how many runnable (non-skipped) rows it has.
  void set_runnable(bool runnable, int rows);
  // The window calls this after a successful start.
  void set_running(bool running);
  bool running() const noexcept { return running_; }
  void show_error(const QString& message);  // empty hides the banner
  void set_confirm(Confirm confirm) { confirm_ = std::move(confirm); }

  // The buttons, for the window's actions and for tests.
  void request_start();
  void request_stop();
  void request_cancel();
  void request_abort();
  void request_truncate();
  void request_resume();
  void request_skip();
  bool start_enabled() const;
  bool stop_enabled() const;
  bool resume_enabled() const;
  bool skip_enabled() const;
  bool holding() const noexcept { return holding_; }

  // For tests.
  QString state_text() const;
  QString progress_text() const;
  QString run_text() const;
  QString wait_text() const;
  QString error_text() const;
  QString spool_text() const;
  // The line for `pending` runs not yet saved: in the spool, or, when the
  // session saves to a database, waiting for it; then, when there is one, why
  // the last was not taken (LabSession::last_save_error), on the same line.
  QString pending_text(std::size_t pending, const std::optional<std::string>& why = {}) const;
  QString notify_text() const;
  int counts_value() const;
  int counts_maximum() const;
  QStringList events() const;  // what each event says, oldest first
  // When each was seen, on the line's clock (simulated time in a simulated
  // session); the table shows the time of day and the date in a tooltip.
  QList<QDateTime> event_times() const;
  QStringList event_time_texts() const;  // the Time column as shown
  // The events table keeps its newest lines only: a queue of a thousand runs
  // says more than ten thousand things, and the list is not their record
  // (the log is).
  static constexpr int kEventLimit = 5000;
  void set_event_limit(int lines);
  QStringList conditionals() const;
  const TimelineModel& timeline() const noexcept { return timeline_; }
  TimelineView* timeline_view() const noexcept { return timeline_view_; }

 signals:
  void startRequested();
  // The queue began or stopped holding, or what it holds for changed:
  // resume_enabled() and skip_enabled() may have changed.
  void holdChanged();

 private:
  void add_event(const QString& line);
  void trim_events();
  void update_buttons();
  void update_progress();
  void set_holding(bool holding, std::optional<std::size_t> row = std::nullopt);
  QString wall_text(TimePoint at);  // `at` on the line's clock, as the time of day

  ExperimentBridge& bridge_;
  Confirm confirm_;
  bool runnable_ = false, running_ = false;
  bool holding_ = false;
  std::optional<std::size_t> held_row_;  // the row held back, when it has not started
  bool skipping_ = false;                // a skip was asked of this hold: leaving it is not "resumed"
  int rows_ = 0, done_ = 0;
  int event_limit_ = kEventLimit;
  QString run_, run_state_, block_;
  std::map<std::string, QString> identifiers_;  // by run id, for the queue being run

  QFrame* banner_;
  QLabel* banner_label_;
  QLabel* state_;
  QLabel* progress_label_;
  QProgressBar* progress_;
  QLabel* run_label_;
  QProgressBar* counts_;
  QLabel* wait_;
  QLabel* spool_;
  QLabel* notify_;
  QPushButton* start_;
  QPushButton* stop_;
  QPushButton* cancel_;
  QPushButton* abort_;
  QPushButton* truncate_;
  QPushButton* resume_;
  QPushButton* skip_;
  QListWidget* conditionals_;
  QTableWidget* events_;
  TimelineModel timeline_;
  TimelineView* timeline_view_;
  QTimer timeline_clock_;  // grows open segments with the line's clock while running
};

}  // namespace pychron::ui
