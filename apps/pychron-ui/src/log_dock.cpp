#include "log_dock.hpp"

#include <chrono>
#include <iterator>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

constexpr LogLevel kLevels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error};

int level_index(LogLevel level) {
  for (int i = 0; i < static_cast<int>(std::size(kLevels)); ++i)
    if (kLevels[i] == level) return i;
  return 0;
}

LogLevel parse_level(const QString& name) {
  for (LogLevel l : kLevels)
    if (log_level_name(l) == name) return l;
  return LogLevel::Info;
}

TimePoint stamp_or_now(TimePoint ts) { return ts == TimePoint{} ? std::chrono::steady_clock::now() : ts; }

}  // namespace

LogDock::LogDock(QWidget* parent)
    : QDockWidget(tr("Log"), parent),
      model_(new LogModel(LogModel::kCapacity, this)),
      proxy_(new LogFilterProxy(this)),
      table_(new QTableView),
      level_combo_(new QComboBox),
      logger_edit_(new QLineEdit),
      text_edit_(new QLineEdit),
      pause_button_(new QToolButton),
      badge_(new QLabel),
      autoscroll_(new QCheckBox(tr("Autoscroll"))),
      flush_timer_(new QTimer(this)) {
  setObjectName(QStringLiteral("LogDock"));
  proxy_->setSourceModel(model_);

  for (LogLevel l : kLevels) level_combo_->addItem(log_level_name(l));
  level_combo_->setToolTip(tr("Minimum level"));
  logger_edit_->setPlaceholderText(tr("logger (prefix or glob)"));
  logger_edit_->setClearButtonEnabled(true);
  text_edit_->setPlaceholderText(tr("message contains"));
  text_edit_->setClearButtonEnabled(true);
  pause_button_->setText(tr("Pause"));
  pause_button_->setCheckable(true);
  badge_->setVisible(false);
  autoscroll_->setChecked(true);

  auto* toolbar = new QToolBar;
  toolbar->setIconSize({16, 16});
  toolbar->addWidget(level_combo_);
  toolbar->addWidget(logger_edit_);
  toolbar->addWidget(text_edit_);
  toolbar->addSeparator();
  toolbar->addWidget(pause_button_);
  toolbar->addWidget(badge_);
  toolbar->addAction(tr("Clear"), this, &LogDock::clear);
  toolbar->addWidget(autoscroll_);
  toolbar->addAction(tr("Save..."), this, &LogDock::save_with_dialog);

  table_->setModel(proxy_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setWordWrap(false);
  table_->setShowGrid(false);
  table_->verticalHeader()->setVisible(false);
  table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table_->verticalHeader()->setDefaultSectionSize(table_->fontMetrics().height() + 4);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setColumnWidth(LogModel::TimeCol, 100);
  table_->setColumnWidth(LogModel::LevelCol, 60);
  table_->setColumnWidth(LogModel::LoggerCol, 140);

  auto* container = new QWidget;
  auto* layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(toolbar);
  layout->addWidget(table_);
  setWidget(container);

  connect(level_combo_, &QComboBox::currentIndexChanged, this, [this](int i) {
    if (i >= 0 && i < static_cast<int>(std::size(kLevels))) proxy_->set_min_level(kLevels[i]);
  });
  connect(logger_edit_, &QLineEdit::textChanged, proxy_, &LogFilterProxy::set_logger_pattern);
  connect(text_edit_, &QLineEdit::textChanged, proxy_, &LogFilterProxy::set_text);
  connect(pause_button_, &QToolButton::toggled, this, &LogDock::set_paused);

  connect(flush_timer_, &QTimer::timeout, this, &LogDock::flush_pending);
  flush_timer_->start(kFlushIntervalMs);
}

void LogDock::append_log(const Log& log) {
  enqueue(LogRecord{stamp_or_now(log.ts), log.level, QString::fromStdString(log.logger),
                    QString::fromStdString(log.message), false});
}

void LogDock::append_line(const QString& line) {
  static const QRegularExpression prefix(QStringLiteral(R"(^(TRACE|DEBUG|INFO|WARN|ERROR) \[([^\]]+)\] )"));
  const auto now = std::chrono::steady_clock::now();
  const auto m = prefix.match(line);
  if (m.hasMatch()) {
    enqueue(LogRecord{now, parse_level(m.captured(1)), m.captured(2), line.mid(m.capturedLength(0)), false});
  } else {
    enqueue(LogRecord{now, LogLevel::Info, QStringLiteral("ui"), line, false});
  }
}

void LogDock::enqueue(LogRecord record) {
  pending_.push_back(std::move(record));
  // Only the newest kCapacity records could ever be shown; bound the buffer.
  while (pending_.size() > static_cast<std::size_t>(LogModel::kCapacity)) pending_.pop_front();
}

void LogDock::flush_pending() {
  if (!paused_ && !pending_.empty()) {
    const QScrollBar* bar = table_->verticalScrollBar();
    const bool at_bottom = bar->value() >= bar->maximum();
    std::vector<LogRecord> batch(std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
    pending_.clear();
    model_->append(std::move(batch));
    if (autoscroll_->isChecked() && at_bottom) table_->scrollToBottom();
  }
  update_badge();
}

void LogDock::flush_if_live() const {
  // line_count()/text() report what the view will show; flushing the buffer
  // here is an observable no-op apart from making the answer current.
  if (!paused_ && !pending_.empty()) const_cast<LogDock*>(this)->flush_pending();
}

void LogDock::update_badge() {
  const bool show = paused_ && !pending_.empty();
  if (show) badge_->setText(tr("+%1 new").arg(pending_.size()));
  badge_->setVisible(show);
}

void LogDock::set_min_level(LogLevel level) {
  const QSignalBlocker block(level_combo_);
  level_combo_->setCurrentIndex(level_index(level));
  proxy_->set_min_level(level);
}

void LogDock::set_logger_filter(const QString& pattern) {
  const QSignalBlocker block(logger_edit_);
  logger_edit_->setText(pattern);
  proxy_->set_logger_pattern(pattern);
}

void LogDock::set_text_filter(const QString& text) {
  const QSignalBlocker block(text_edit_);
  text_edit_->setText(text);
  proxy_->set_text(text);
}

void LogDock::set_paused(bool paused) {
  if (pause_button_->isChecked() != paused) {
    const QSignalBlocker block(pause_button_);
    pause_button_->setChecked(paused);
  }
  paused_ = paused;
  update_badge();
}

void LogDock::clear() {
  pending_.clear();
  model_->clear();
  update_badge();
}

int LogDock::line_count() const {
  flush_if_live();
  return proxy_->rowCount();
}

QString LogDock::format_row(int proxy_row) const {
  const LogRecord& r = model_->record(proxy_->mapToSource(proxy_->index(proxy_row, 0)).row());
  return log_wall_time(r.ts).toString(QStringLiteral("HH:mm:ss ")) + log_level_name(r.level) + QStringLiteral(" [") +
         r.logger + QStringLiteral("] ") + r.message;
}

QString LogDock::text() const {
  flush_if_live();
  QStringList lines;
  const int rows = proxy_->rowCount();
  lines.reserve(rows);
  for (int i = 0; i < rows; ++i) lines.append(format_row(i));
  return lines.join(QLatin1Char('\n'));
}

bool LogDock::save_visible(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
  const int rows = proxy_->rowCount();
  for (int i = 0; i < rows; ++i) {
    const QByteArray line = (format_row(i) + QLatin1Char('\n')).toUtf8();
    if (file.write(line) != line.size()) return false;
  }
  return file.flush();
}

void LogDock::save_with_dialog() {
  const QString path = QFileDialog::getSaveFileName(this, tr("Save log"), QStringLiteral("pychron-log.txt"),
                                                    tr("Text files (*.txt *.log);;All files (*)"));
  if (path.isEmpty()) return;
  if (!save_visible(path)) QMessageBox::warning(this, tr("Save log"), tr("Could not write %1").arg(path));
}

}  // namespace pychron::ui
