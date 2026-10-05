#pragma once

// ExecutorPane (experiment-window design 5.3): what the executor is doing and
// the controls to start and stop it.
//
//   state, runs done n/N, progress over the runnable rows
//   current run: identifier, run state, block, counts bar
//   the executor's current wait (reason and duration)
//   Start / Stop / Cancel / Abort / Truncate
//   conditionals that tripped; one line per run start/finish, thing a run
//   said (a script's info(), whether a hole was centred), queue edit,
//   peak-center result and queue end; records left in the spool
//   what the session cannot do (LabSession::problems), from the start
//
// Start is the window's to carry out (it knows the queue and the selected
// row): the pane emits startRequested. The other controls go straight to the
// bridge. Cancel and Abort ask first (default No).

#include <functional>
#include <map>
#include <string>

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
  bool start_enabled() const;
  bool stop_enabled() const;

  // For tests.
  QString state_text() const;
  QString progress_text() const;
  QString run_text() const;
  QString wait_text() const;
  QString error_text() const;
  QString spool_text() const;
  QString notify_text() const;
  int counts_value() const;
  int counts_maximum() const;
  QStringList events() const;
  QStringList conditionals() const;
  const TimelineModel& timeline() const noexcept { return timeline_; }
  TimelineView* timeline_view() const noexcept { return timeline_view_; }

 signals:
  void startRequested();

 private:
  void add_event(const QString& line);
  void update_buttons();
  void update_progress();

  ExperimentBridge& bridge_;
  Confirm confirm_;
  bool runnable_ = false, running_ = false;
  int rows_ = 0, done_ = 0;
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
  QListWidget* conditionals_;
  QListWidget* events_;
  TimelineModel timeline_;
  TimelineView* timeline_view_;
  QTimer timeline_clock_;  // grows open segments with the line's clock while running
};

}  // namespace pychron::ui
