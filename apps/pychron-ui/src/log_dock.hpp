#pragma once

// LogDock: filterable table of SignalBus Log events and UI messages
// (spec section 5). Incoming records are buffered and inserted into the
// LogModel ring in batches by a 30 Hz timer, so a flood cannot stall the UI.

#include <deque>
#include <filesystem>
#include <functional>
#include <string>

#include <QDockWidget>
#include <QString>

#include "log_filter_proxy.hpp"
#include "log_model.hpp"
#include "pychron/core/events.hpp"

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QTimer;
class QToolButton;

namespace pychron::ui {

class LogDock : public QDockWidget {
  Q_OBJECT

 public:
  static constexpr int kMaxLines = LogModel::kCapacity;
  static constexpr int kFlushIntervalMs = 33;  // ~30 Hz

  using LevelCallback = std::function<void(std::string pattern, LogLevel level)>;
  static constexpr int kMaxHistoryLineBytes = 4096;

  explicit LogDock(QWidget* parent = nullptr);

  void append_log(const Log& log);
  // Parses a leading "LEVEL [logger] " prefix; otherwise an info record from "ui".
  void append_line(const QString& line);

  // Visible (filtered) rows. When not paused, buffered records are flushed
  // first so the answer reflects everything appended so far.
  int line_count() const;
  // Visible rows as "HH:mm:ss LEVEL [logger] message", newline separated.
  QString text() const;

  void set_min_level(LogLevel level);
  void set_logger_filter(const QString& pattern);
  void set_text_filter(const QString& text);
  void set_paused(bool paused);
  bool paused() const noexcept { return paused_; }
  int pending_count() const noexcept { return static_cast<int>(pending_.size()); }
  // Moves buffered records into the model (no-op while paused). Called by the
  // 30 Hz timer; tests call it directly.
  void flush_pending();
  // Writes the visible rows (one per line) to `path`. False on I/O failure.
  bool save_visible(const QString& path);
  void clear();

  // "Set logger level..." toolbar action; hidden until a callback is set.
  void set_level_callback(LevelCallback callback);
  QAction* level_action() const noexcept { return level_action_; }
  // Testable core of the level dialog: forwards (trimmed pattern, level) to
  // the callback. An empty pattern (or no callback) is ignored.
  void apply_level(const QString& pattern, LogLevel level);

  // Loads the last `max_lines` lines of `log_file` (the LogHub file format,
  // UTC timestamps) as dimmed history records, inserted directly into the
  // model. Reads only a bounded tail of the file; lines are capped at
  // kMaxHistoryLineBytes. Unparseable lines become info records from logger
  // "file". Returns the number of records added; 0 if the file is missing.
  int load_history(const std::filesystem::path& log_file, int max_lines = 1000);

  const LogModel* model() const noexcept { return model_; }

 private:
  void enqueue(LogRecord record);
  // The flush itself; const so line_count()/text() can make their answer
  // current (only the mutable pending buffer and the owned model change).
  void flush_now() const;
  void update_badge() const;
  void save_with_dialog();
  void level_with_dialog();
  QString format_row(int proxy_row) const;

  LogModel* model_;
  LogFilterProxy* proxy_;
  QTableView* table_;
  QComboBox* level_combo_;
  QLineEdit* logger_edit_;
  QLineEdit* text_edit_;
  QToolButton* pause_button_;
  QLabel* badge_;
  QAction* badge_action_ = nullptr;
  QAction* level_action_ = nullptr;
  QCheckBox* autoscroll_;
  QTimer* flush_timer_;
  mutable std::deque<LogRecord> pending_;
  bool paused_ = false;
  LevelCallback level_callback_;
};

}  // namespace pychron::ui
