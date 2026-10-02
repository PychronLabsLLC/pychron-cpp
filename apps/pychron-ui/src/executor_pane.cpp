#include "executor_pane.hpp"

#include <cmath>

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

namespace exec = experiment::executor;
namespace meas = experiment::measurement;

QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString clock_text(experiment::Duration d) {
  const auto total = static_cast<long long>(std::llround(d.count()));
  return QStringLiteral("%1:%2:%3")
      .arg(total / 3600)
      .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

ExecutorPane::ExecutorPane(ExperimentBridge& bridge, QWidget* parent)
    : QWidget(parent),
      bridge_(bridge),
      banner_(new QFrame),
      banner_label_(new QLabel),
      state_(new QLabel),
      progress_label_(new QLabel),
      progress_(new QProgressBar),
      run_label_(new QLabel),
      counts_(new QProgressBar),
      wait_(new QLabel),
      spool_(new QLabel),
      start_(new QPushButton(tr("Start"))),
      stop_(new QPushButton(tr("Stop"))),
      cancel_(new QPushButton(tr("Cancel"))),
      abort_(new QPushButton(tr("Abort"))),
      truncate_(new QPushButton(tr("Truncate"))),
      conditionals_(new QListWidget),
      events_(new QListWidget) {
  banner_->setObjectName(QStringLiteral("ExecutorBanner"));
  banner_->setStyleSheet(QStringLiteral("#ExecutorBanner { background: #f8d7da; } #ExecutorBanner QLabel { color: #721c24; }"));
  auto* banner_row = new QHBoxLayout(banner_);
  banner_label_->setWordWrap(true);
  banner_row->addWidget(banner_label_);
  banner_->hide();

  start_->setToolTip(tr("Start the queue at the selected row"));
  stop_->setToolTip(tr("Finish the current run, then stop"));
  cancel_->setToolTip(tr("Cancel the current run and end the queue"));
  abort_->setToolTip(tr("Abort the current run immediately and end the queue"));
  truncate_->setToolTip(tr("End the current measurement block early"));
  auto* buttons = new QHBoxLayout;
  for (auto* b : {start_, stop_, cancel_, abort_, truncate_}) buttons->addWidget(b);
  buttons->addStretch(1);

  auto* status = new QGridLayout;
  status->addWidget(new QLabel(tr("Executor")), 0, 0);
  status->addWidget(state_, 0, 1);
  status->addWidget(progress_label_, 0, 2);
  status->addWidget(progress_, 0, 3);
  status->addWidget(new QLabel(tr("Run")), 1, 0);
  status->addWidget(run_label_, 1, 1, 1, 2);
  status->addWidget(counts_, 1, 3);
  status->addWidget(new QLabel(tr("Waiting")), 2, 0);
  status->addWidget(wait_, 2, 1, 1, 3);
  status->addWidget(spool_, 3, 0, 1, 4);
  status->setColumnStretch(3, 1);
  counts_->setFormat(QStringLiteral("%v/%m"));
  counts_->setRange(0, 1);
  counts_->setValue(0);
  progress_->setFormat(QStringLiteral("%v/%m"));
  spool_->hide();

  auto* lists = new QSplitter(Qt::Horizontal);
  auto* cond_box = new QWidget;
  auto* cond_col = new QVBoxLayout(cond_box);
  cond_col->setContentsMargins(0, 0, 0, 0);
  cond_col->addWidget(new QLabel(tr("Conditionals")));
  cond_col->addWidget(conditionals_);
  auto* event_box = new QWidget;
  auto* event_col = new QVBoxLayout(event_box);
  event_col->setContentsMargins(0, 0, 0, 0);
  event_col->addWidget(new QLabel(tr("Events")));
  event_col->addWidget(events_);
  lists->addWidget(event_box);
  lists->addWidget(cond_box);
  lists->setStretchFactor(0, 2);
  lists->setStretchFactor(1, 1);

  auto* column = new QVBoxLayout(this);
  column->addWidget(banner_);
  column->addLayout(buttons);
  column->addLayout(status);
  column->addWidget(lists, 1);

  confirm_ = [this](const QString& title, const QString& question) {
    return QMessageBox::question(this, title, question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
           QMessageBox::Yes;
  };

  connect(start_, &QPushButton::clicked, this, [this] { request_start(); });
  connect(stop_, &QPushButton::clicked, this, [this] { request_stop(); });
  connect(cancel_, &QPushButton::clicked, this, [this] { request_cancel(); });
  connect(abort_, &QPushButton::clicked, this, [this] { request_abort(); });
  connect(truncate_, &QPushButton::clicked, this, [this] { request_truncate(); });

  connect(&bridge_, &ExperimentBridge::executorStateChanged, this, [this](const exec::ExecutorStateChanged& e) {
    state_->setText(q(exec::to_string(e.to)));
    if (e.to != exec::ExecutorState::Preparing) wait_->clear();
  });
  connect(&bridge_, &ExperimentBridge::runStarted, this, [this](const exec::RunStarted& e) {
    run_ = q(e.identifier);
    run_state_.clear();
    block_.clear();
    run_label_->setText(run_);
    counts_->setRange(0, 1);
    counts_->setValue(0);
    wait_->clear();
    add_event(tr("run %1 %2 started").arg(e.row).arg(run_));
  });
  connect(&bridge_, &ExperimentBridge::runStateChanged, this, [this](const experiment::run::RunStateChanged& e) {
    run_state_ = q(experiment::run::to_string(e.to));
    wait_->clear();  // the run moved on, so whatever it waited for arrived
    run_label_->setText(QStringLiteral("%1 — %2%3").arg(run_, run_state_, block_.isEmpty() ? QString() : QStringLiteral(" — ") + block_));
  });
  connect(&bridge_, &ExperimentBridge::blockStarted, this, [this](const meas::BlockStarted& e) {
    block_ = q(meas::to_string(e.block));
    wait_->clear();
    counts_->setRange(0, 1);  // a block without counts (peak center) shows empty
    counts_->setValue(0);
    run_label_->setText(QStringLiteral("%1 — %2 — %3").arg(run_, run_state_, block_));
  });
  connect(&bridge_, &ExperimentBridge::countsProgress, this, [this](const meas::CountsProgress& e) {
    counts_->setRange(0, std::max(1, e.n));
    counts_->setValue(std::min(e.i, std::max(1, e.n)));
  });
  connect(&bridge_, &ExperimentBridge::executorWaiting, this, [this](const exec::ExecutorWaiting& e) {
    wait_->setText(e.duration > experiment::Duration::zero()
                       ? QStringLiteral("%1 (%2)").arg(q(e.reason), clock_text(e.duration))
                       : q(e.reason));
  });
  connect(&bridge_, &ExperimentBridge::runFinished, this, [this](const exec::RunFinished& e) {
    const auto& s = e.summary;
    QString line = tr("run %1 %2-%3%4: %5")
                       .arg(s.row)
                       .arg(q(s.identifier))
                       .arg(s.aliquot)
                       .arg(q(s.step), q(experiment::run::to_string(s.state)));
    if (s.truncated) line += tr(" (truncated)");
    if (s.error) line += QStringLiteral(": ") + q(*s.error);
    add_event(line);
    ++done_;
    update_progress();
  });
  connect(&bridge_, &ExperimentBridge::queueEdited, this, [this](const exec::QueueEdited& e) {
    for (const auto& c : e.changes) add_event(tr("queue: %1").arg(q(c)));
  });
  connect(&bridge_, &ExperimentBridge::conditionalTripped, this, [this](const meas::ConditionalTripped& e) {
    QString line = QStringLiteral("%1  %2: %3").arg(run_, q(e.trip.name), q(e.trip.check));
    conditionals_->addItem(line);
    conditionals_->scrollToBottom();
  });
  connect(&bridge_, &ExperimentBridge::peakCenterDone, this, [this](const jobs::PeakCenterDone& e) {
    const auto& r = e.result;
    add_event(r.ok && r.center ? tr("peak center %1 on %2: %3").arg(q(r.isotope), q(r.detector)).arg(*r.center, 0, 'f', 6)
                               : tr("peak center %1 on %2 failed: %3").arg(q(r.isotope), q(r.detector), q(r.message)));
  });
  connect(&bridge_, &ExperimentBridge::queueEnded, this, [this](const experiment::lab::QueueEnded& e) {
    const auto& r = e.result;
    add_event(tr("queue %1%2").arg(q(exec::to_string(r.end)), r.reason.empty() ? QString() : QStringLiteral(": ") + q(r.reason)));
    wait_->clear();
    state_->setText(q(exec::to_string(exec::ExecutorState::Idle)));
    const auto pending = bridge_.session().pending_saves();
    spool_->setText(tr("%n record(s) in the spool", nullptr, static_cast<int>(pending)));
    spool_->setVisible(pending > 0);
    set_running(false);
  });

  state_->setText(q(exec::to_string(exec::ExecutorState::Idle)));
  update_progress();
  update_buttons();
}

void ExecutorPane::set_runnable(bool runnable, int rows) {
  runnable_ = runnable;
  if (!running_) rows_ = rows;
  update_progress();
  update_buttons();
}

void ExecutorPane::set_running(bool running) {
  running_ = running;
  if (running) {
    done_ = 0;
    conditionals_->clear();
    show_error({});
    spool_->hide();
  }
  update_progress();
  update_buttons();
}

void ExecutorPane::show_error(const QString& message) {
  banner_label_->setText(message);
  banner_->setVisible(!message.isEmpty());
}

void ExecutorPane::request_start() {
  if (start_enabled()) emit startRequested();
}

void ExecutorPane::request_stop() {
  if (!running_) return;
  bridge_.stop();
  add_event(tr("stop requested: the queue ends after the current run"));
}

void ExecutorPane::request_cancel() {
  if (!running_) return;
  if (!confirm_(tr("Cancel"), tr("Cancel the current run and end the queue?"))) return;
  bridge_.cancel();
  add_event(tr("cancel requested"));
}

void ExecutorPane::request_abort() {
  if (!running_) return;
  if (!confirm_(tr("Abort"), tr("Abort the current run immediately and end the queue?"))) return;
  bridge_.abort();
  add_event(tr("abort requested"));
}

void ExecutorPane::request_truncate() {
  if (!running_) return;
  bridge_.truncate();
  add_event(tr("truncate requested"));
}

bool ExecutorPane::start_enabled() const { return !running_ && runnable_; }
bool ExecutorPane::stop_enabled() const { return running_; }

void ExecutorPane::add_event(const QString& line) {
  events_->addItem(line);
  events_->scrollToBottom();
}

void ExecutorPane::update_buttons() {
  start_->setEnabled(start_enabled());
  for (auto* b : {stop_, cancel_, abort_, truncate_}) b->setEnabled(running_);
}

void ExecutorPane::update_progress() {
  progress_->setRange(0, std::max(1, rows_));
  progress_->setValue(std::min(done_, std::max(1, rows_)));
  progress_label_->setText(tr("%1/%2 run(s)").arg(done_).arg(rows_));
}

QString ExecutorPane::state_text() const { return state_->text(); }
QString ExecutorPane::progress_text() const { return progress_label_->text(); }
QString ExecutorPane::run_text() const { return run_label_->text(); }
QString ExecutorPane::wait_text() const { return wait_->text(); }
QString ExecutorPane::error_text() const { return banner_->isHidden() ? QString() : banner_label_->text(); }
QString ExecutorPane::spool_text() const { return spool_->isHidden() ? QString() : spool_->text(); }
int ExecutorPane::counts_value() const { return counts_->value(); }
int ExecutorPane::counts_maximum() const { return counts_->maximum(); }

QStringList ExecutorPane::events() const {
  QStringList out;
  for (int i = 0; i < events_->count(); ++i) out.append(events_->item(i)->text());
  return out;
}

QStringList ExecutorPane::conditionals() const {
  QStringList out;
  for (int i = 0; i < conditionals_->count(); ++i) out.append(conditionals_->item(i)->text());
  return out;
}

}  // namespace pychron::ui
