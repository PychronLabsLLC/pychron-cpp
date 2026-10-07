#pragma once

// AlarmDock: active alarms, one row per source (a repeat alarm updates the
// row). Acknowledging removes rows locally; nothing is sent to the core (M1).

#include <functional>
#include <string>

#include <QDateTime>
#include <QDockWidget>
#include <QPushButton>
#include <QTreeWidget>

#include "pychron/core/events.hpp"

namespace pychron::ui {

class AlarmDock : public QDockWidget {
  Q_OBJECT

 public:
  // `now` is the time a row is stamped with; the real time when empty. The
  // main window gives the line's clock, which is not real time on a
  // simulated line.
  explicit AlarmDock(QWidget* parent = nullptr, std::function<QDateTime()> now = {});

  void add_alarm(const Alarm& alarm);
  // Removes the selected rows; returns how many were acknowledged.
  int acknowledge_selected();
  void acknowledge_all();

  int active_count() const;
  bool is_active(const std::string& source) const;
  QTreeWidget* tree() const noexcept { return tree_; }

 private:
  QTreeWidgetItem* row_for(const QString& source) const;

  std::function<QDateTime()> now_;
  QTreeWidget* tree_;
  QPushButton* ack_;
  QPushButton* ack_all_;
};

}  // namespace pychron::ui
