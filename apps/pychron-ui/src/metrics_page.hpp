#pragma once

// File > Preferences, Metrics: the line's `[metrics]` on this computer, the
// endpoint the lab's monitoring box reads (docs/observability.md). All of it
// is read when the application starts.

#include <QWidget>

#include "pychron/core/config/metrics_config.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace pychron::ui {

class MetricsPage : public QWidget {
  Q_OBJECT

 public:
  // Who may reach the endpoint, as `bind` says it.
  enum Where { ThisComputer = 0, AllAddresses = 1, OneAddress = 2 };

  explicit MetricsPage(QWidget* parent = nullptr);

  // `status`: what the endpoint is doing now.
  void set(const config::MetricsConfig& metrics, const QString& status);
  config::MetricsConfig value() const;
  // What is wrong with the fields, for the user; empty when nothing is.
  QString problem() const;

  QCheckBox* enabled() const noexcept { return enabled_; }
  QComboBox* where() const noexcept { return where_; }
  QLineEdit* address() const noexcept { return address_; }
  QSpinBox* port() const noexcept { return port_; }
  QLabel* status() const noexcept { return status_; }

 private:
  config::MetricsConfig base_;
  QCheckBox* enabled_;
  QComboBox* where_;
  QLineEdit* address_;
  QSpinBox* port_;
  QLabel* status_;
};

}  // namespace pychron::ui
