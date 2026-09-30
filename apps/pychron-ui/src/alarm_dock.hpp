#pragma once

// AlarmDock: active alarms, one row per source (a repeat alarm updates the
// row). Acknowledging removes rows locally; nothing is sent to the core (M1).

#include <string>

#include <QDockWidget>
#include <QPushButton>
#include <QTreeWidget>

#include "pychron/core/events.hpp"

namespace pychron::ui {

class AlarmDock : public QDockWidget {
  Q_OBJECT

 public:
  explicit AlarmDock(QWidget* parent = nullptr);

  void add_alarm(const Alarm& alarm);
  // Removes the selected rows; returns how many were acknowledged.
  int acknowledge_selected();
  void acknowledge_all();

  int active_count() const;
  bool is_active(const std::string& source) const;
  QTreeWidget* tree() const noexcept { return tree_; }

 private:
  QTreeWidgetItem* row_for(const QString& source) const;

  QTreeWidget* tree_;
  QPushButton* ack_;
  QPushButton* ack_all_;
};

}  // namespace pychron::ui
