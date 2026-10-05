#pragma once

// HeaterDock (plan 2026-10-05, task E3; legacy HeaterManager): one row per
// [[heaters]] entry with an on/off button (confirmed first, as legacy), a
// Use PID box, a setpoint field applied on Enter, and the readback; below
// them a strip chart of every heater's readback. A control whose field the
// heater's driver does not have is disabled. Everything goes through the
// CoreBridge: rows follow its HeaterSample relay, and commands run on its
// executor.

#include <functional>
#include <map>
#include <memory>
#include <string>

#include <QCheckBox>
#include <QDockWidget>
#include <QLCDNumber>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "core_bridge.hpp"
#include "strip_chart_model.hpp"
#include "strip_chart_view.hpp"

namespace pychron::ui {

class HeaterDock : public QDockWidget {
  Q_OBJECT

 public:
  // Asked before a heater is switched; true goes ahead.
  using Confirm = std::function<bool(const QString& question)>;

  // `bridge` must outlive the dock. The default confirm is a Yes/No box.
  explicit HeaterDock(CoreBridge& bridge, QWidget* parent = nullptr);

  void set_confirm(Confirm confirm) { confirm_ = std::move(confirm); }

  QPushButton* power_button(const std::string& heater) const;
  QCheckBox* pid_box(const std::string& heater) const;
  QLineEdit* setpoint_field(const std::string& heater) const;
  QLCDNumber* readback(const std::string& heater) const;
  // The last failure, empty after a success.
  QString status_text() const;
  const StripChartModel& chart_model() const noexcept { return *model_; }
  StripChartView* chart() const noexcept { return chart_; }

 signals:
  void heaterFailed(const QString& message);

 private:
  struct Row {
    QPushButton* power = nullptr;
    QCheckBox* pid = nullptr;
    QLineEdit* setpoint = nullptr;
    QLCDNumber* readback = nullptr;
    bool pending = false;
  };

  void show_sample(const HeaterSample& sample);
  void on_power(const std::string& heater);
  void on_finished(const QString& heater, const Result<void>& result);
  void set_pending(Row& row, bool pending);
  const Row* row(const std::string& heater) const;

  CoreBridge& bridge_;
  Confirm confirm_;
  std::unique_ptr<StripChartModel> model_;
  StripChartView* chart_ = nullptr;
  std::map<std::string, Row> rows_;
  QLabel* status_ = nullptr;
};

}  // namespace pychron::ui
