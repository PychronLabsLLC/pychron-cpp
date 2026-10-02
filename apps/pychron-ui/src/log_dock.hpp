#pragma once

// LogDock: filterable table of SignalBus Log events and UI messages
// (spec section 5). Incoming records are buffered and inserted into the
// LogModel ring in batches by a 30 Hz timer, so a flood cannot stall the UI.

#include <deque>

#include <QDockWidget>
#include <QString>

#include "log_filter_proxy.hpp"
#include "log_model.hpp"
#include "pychron/core/events.hpp"

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

 private:
  void enqueue(LogRecord record);
  void update_badge();
  void save_with_dialog();
  void flush_if_live() const;
  QString format_row(int proxy_row) const;

  LogModel* model_;
  LogFilterProxy* proxy_;
  QTableView* table_;
  QComboBox* level_combo_;
  QLineEdit* logger_edit_;
  QLineEdit* text_edit_;
  QToolButton* pause_button_;
  QLabel* badge_;
  QCheckBox* autoscroll_;
  QTimer* flush_timer_;
  std::deque<LogRecord> pending_;
  bool paused_ = false;
};

}  // namespace pychron::ui
