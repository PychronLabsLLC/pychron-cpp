#pragma once

// File > Preferences, Logging: the line's `[logging]` on this computer. The
// levels take effect when applied; where the log file goes and how it is
// kept are read when the application starts.

#include <QWidget>

#include "pychron/core/config/logging_config.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace pychron::ui {

class LoggingPage : public QWidget {
  Q_OBJECT

 public:
  explicit LoggingPage(QWidget* parent = nullptr);

  void set(const config::LoggingConfig& logging);
  // What the fields hold. A row with no pattern is not a level; of two rows
  // with one pattern the lower wins.
  config::LoggingConfig value() const;

  QComboBox* default_level() const noexcept { return default_level_; }
  QTableWidget* levels() const noexcept { return levels_; }  // pattern, level
  QComboBox* level_at(int row) const;
  QPushButton* add_level() const noexcept { return add_; }
  QPushButton* remove_level() const noexcept { return remove_; }
  QLineEdit* folder() const noexcept { return folder_; }
  QSpinBox* max_size() const noexcept { return max_size_; }
  QSpinBox* max_files() const noexcept { return max_files_; }
  QCheckBox* echo() const noexcept { return echo_; }

 private:
  void add_row(const QString& pattern, LogLevel level);

  config::LoggingConfig base_;  // what set() was given: the fields overwrite a copy of it
  QComboBox* default_level_;
  QTableWidget* levels_;
  QPushButton* add_;
  QPushButton* remove_;
  QLineEdit* folder_;
  QSpinBox* max_size_;
  QSpinBox* max_files_;
  QCheckBox* echo_;
};

}  // namespace pychron::ui
