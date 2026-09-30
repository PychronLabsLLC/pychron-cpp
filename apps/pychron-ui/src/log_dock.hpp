#pragma once

// LogDock: tails SignalBus Log events and actuation errors.

#include <QDockWidget>
#include <QPlainTextEdit>

#include "pychron/core/events.hpp"

namespace pychron::ui {

class LogDock : public QDockWidget {
  Q_OBJECT

 public:
  static constexpr int kMaxLines = 2000;

  explicit LogDock(QWidget* parent = nullptr);

  void append_log(const Log& log);
  void append_line(const QString& line);
  int line_count() const;
  QString text() const;

 private:
  QPlainTextEdit* view_;
};

}  // namespace pychron::ui
