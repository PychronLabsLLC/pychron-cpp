#include "log_dock.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
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
  for (int i = 0; std::cmp_less(i, std::size(kLevels)); ++i)
    if (kLevels[i] == level) return i;
  return 0;
}

LogLevel parse_level(const QString& name) {
  for (LogLevel l : kLevels)
    if (log_level_name(l) == name) return l;
  return LogLevel::Info;
}

TimePoint stamp_or_now(TimePoint ts) { return ts == TimePoint{} ? std::chrono::steady_clock::now() : ts; }

// The last `max_lines` lines of `path`, each cut to `max_line` bytes, read
// without loading the file: a backwards scan bounded to
// max_lines * (max_line + 1) bytes finds the first wanted line, then only
// that tail is read. A line longer than the budget is dropped.
std::vector<std::string> read_tail_lines(const std::filesystem::path& path, int max_lines, std::size_t max_line) {
  std::vector<std::string> lines;
  std::error_code ec;
  if (max_lines <= 0 || path.empty() || !std::filesystem::is_regular_file(path, ec)) return lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) return lines;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return lines;

  const auto byte_at = [&in](std::streamoff at) {
    char c = 0;
    in.seekg(at);
    in.get(c);
    return c;
  };
  const std::streamoff budget = static_cast<std::streamoff>(max_lines) * static_cast<std::streamoff>(max_line + 1);
  const std::streamoff floor = size > budget ? size - budget : 0;
  // A final newline ends the last line; it does not begin another one.
  const std::streamoff scan_end = byte_at(size - 1) == '\n' ? size - 1 : size;

  constexpr std::streamoff kChunk = 64 * 1024;
  std::vector<char> chunk;
  std::streamoff pos = scan_end;
  std::streamoff start = floor;
  bool found = false;
  int newlines = 0;
  while (pos > floor && !found) {
    const std::streamoff n = std::min(kChunk, pos - floor);
    pos -= n;
    chunk.resize(static_cast<std::size_t>(n));
    in.seekg(pos);
    if (!in.read(chunk.data(), n)) return lines;
    for (std::streamoff i = n; i-- > 0;) {
      if (chunk[static_cast<std::size_t>(i)] == '\n' && ++newlines == max_lines) {
        start = pos + i + 1;
        found = true;
        break;
      }
    }
  }
  // Budget exhausted mid-line: the line straddling `floor` is incomplete.
  const bool skip_first = !found && floor > 0 && byte_at(floor - 1) != '\n';

  std::string tail(static_cast<std::size_t>(size - start), '\0');
  in.seekg(start);
  if (!in.read(tail.data(), static_cast<std::streamsize>(tail.size()))) return lines;

  std::size_t begin = 0;
  bool first = true;
  while (begin < tail.size()) {
    std::size_t end = tail.find('\n', begin);
    if (end == std::string::npos) end = tail.size();
    if (!(first && skip_first)) {
      std::string_view line(tail.data() + begin, end - begin);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (!line.empty()) lines.emplace_back(line.substr(0, max_line));
    }
    first = false;
    begin = end + 1;
  }
  if (lines.size() > static_cast<std::size_t>(max_lines))
    lines.erase(lines.begin(), lines.end() - max_lines);
  return lines;
}

LogLevel parse_file_level(const QString& name) {
  for (LogLevel l : kLevels)
    if (log_level_name(l).compare(name, Qt::CaseInsensitive) == 0) return l;
  return LogLevel::Info;
}

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
  // Fix the steady->wall anchor now, not at the first displayed record.
  static_cast<void>(log_wall_time(std::chrono::steady_clock::now()));
  proxy_->setSourceModel(model_);

  for (LogLevel l : kLevels) level_combo_->addItem(log_level_name(l));
  level_combo_->setToolTip(tr("Minimum level"));
  logger_edit_->setPlaceholderText(tr("logger (prefix or glob)"));
  logger_edit_->setClearButtonEnabled(true);
  text_edit_->setPlaceholderText(tr("message contains"));
  text_edit_->setClearButtonEnabled(true);
  pause_button_->setText(tr("Pause"));
  pause_button_->setCheckable(true);
  autoscroll_->setChecked(true);

  auto* toolbar = new QToolBar;
  toolbar->setIconSize({16, 16});
  toolbar->addWidget(level_combo_);
  toolbar->addWidget(logger_edit_);
  toolbar->addWidget(text_edit_);
  toolbar->addSeparator();
  toolbar->addWidget(pause_button_);
  // Widgets in a toolbar are shown/hidden through their QAction.
  badge_action_ = toolbar->addWidget(badge_);
  badge_action_->setVisible(false);
  toolbar->addAction(tr("Clear"), this, &LogDock::clear);
  toolbar->addWidget(autoscroll_);
  toolbar->addAction(tr("Save..."), this, &LogDock::save_with_dialog);
  level_action_ = toolbar->addAction(tr("Set logger level..."), this, &LogDock::level_with_dialog);
  level_action_->setVisible(false);

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
    if (i >= 0 && std::cmp_less(i, std::size(kLevels))) proxy_->set_min_level(kLevels[i]);
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

void LogDock::flush_pending() { flush_now(); }

void LogDock::flush_now() const {
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

void LogDock::update_badge() const {
  const bool show = paused_ && !pending_.empty();
  if (show) badge_->setText(tr("+%1 new").arg(pending_.size()));
  badge_action_->setVisible(show);
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
  flush_now();
  return proxy_->rowCount();
}

QString LogDock::format_row(int proxy_row) const {
  const LogRecord& r = model_->record(proxy_->mapToSource(proxy_->index(proxy_row, 0)).row());
  return log_wall_time(r.ts).toString(QStringLiteral("HH:mm:ss ")) + log_level_name(r.level) + QStringLiteral(" [") +
         r.logger + QStringLiteral("] ") + r.message;
}

QString LogDock::text() const {
  flush_now();
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

void LogDock::set_level_callback(LevelCallback callback) {
  level_callback_ = std::move(callback);
  level_action_->setVisible(static_cast<bool>(level_callback_));
}

void LogDock::apply_level(const QString& pattern, LogLevel level) {
  const QString p = pattern.trimmed();
  if (p.isEmpty() || !level_callback_) return;
  level_callback_(p.toStdString(), level);
  enqueue(LogRecord{std::chrono::steady_clock::now(), LogLevel::Info, QStringLiteral("ui"),
                    tr("logger level for '%1' set to %2").arg(p, log_level_name(level)), false});
}

void LogDock::level_with_dialog() {
  bool ok = false;
  const QString pattern =
      QInputDialog::getText(this, tr("Set logger level"), tr("Logger pattern (name or glob, e.g. transport.*):"),
                            QLineEdit::Normal, logger_edit_->text(), &ok);
  if (!ok || pattern.trimmed().isEmpty()) return;
  QStringList names;
  for (LogLevel l : kLevels) names.append(log_level_name(l));
  const QString name = QInputDialog::getItem(this, tr("Set logger level"), tr("Level for %1:").arg(pattern.trimmed()),
                                             names, level_index(LogLevel::Info), false, &ok);
  if (!ok) return;
  apply_level(pattern, parse_level(name));
}

int LogDock::load_history(const std::filesystem::path& log_file, int max_lines) {
  // `2026-10-01T14:03:22.481Z [warn] transport.serial.ig1: msg`
  static const QRegularExpression re(QStringLiteral(
      R"(^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z) \[(trace|debug|info|warn|error)\] (\S+): (.*)$)"));
  const auto lines = read_tail_lines(log_file, std::min(max_lines, LogModel::kCapacity),
                                     static_cast<std::size_t>(kMaxHistoryLineBytes));
  if (lines.empty()) return 0;

  std::vector<LogRecord> records;
  records.reserve(lines.size());
  TimePoint last_ts = std::chrono::steady_clock::now();
  bool have_ts = false;
  for (const std::string& raw : lines) {
    const QString line = QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size()));
    const auto m = re.match(line);
    if (m.hasMatch()) {
      const QDateTime wall = QDateTime::fromString(m.captured(1), Qt::ISODateWithMs);
      if (wall.isValid()) {
        last_ts = log_steady_time(wall);
        have_ts = true;
        records.push_back(LogRecord{last_ts, parse_file_level(m.captured(2)), m.captured(3), m.captured(4), true});
        continue;
      }
    }
    // Continuation or foreign line: keep it, next to its neighbour in time.
    records.push_back(LogRecord{have_ts ? last_ts : std::chrono::steady_clock::now(), LogLevel::Info,
                                QStringLiteral("file"), line, true});
  }
  const int count = static_cast<int>(records.size());
  model_->append(std::move(records));
  return count;
}

void LogDock::save_with_dialog() {
  const QString path = QFileDialog::getSaveFileName(this, tr("Save log"), QStringLiteral("pychron-log.txt"),
                                                    tr("Text files (*.txt *.log);;All files (*)"));
  if (path.isEmpty()) return;
  if (!save_visible(path)) QMessageBox::warning(this, tr("Save log"), tr("Could not write %1").arg(path));
}

}  // namespace pychron::ui
