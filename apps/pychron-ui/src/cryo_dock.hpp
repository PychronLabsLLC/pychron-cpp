#pragma once

// CryoDock (plan 2026-10-05, task C6): the line's cryostat. One row per input
// with its latest temperature, one per control loop with a setpoint field, a
// Set button and the setpoint the controller reports, and a strip chart of
// every input. Everything goes through the CoreBridge: temperatures are its
// TemperatureSample relay, setpoints its executor commands.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>

#include "core_bridge.hpp"
#include "strip_chart_model.hpp"
#include "strip_chart_view.hpp"

namespace pychron::ui {

class CryoDock : public QDockWidget {
  Q_OBJECT

 public:
  // `bridge` must outlive the dock. Every output's setpoint is read back
  // once the line has started, and again after each Snapshot.
  explicit CryoDock(CoreBridge& bridge, QWidget* parent = nullptr);

  void read_setpoints();

  // Sends output `output`'s field to the controller.
  void apply_setpoint(int output);

  QString temperature_text(const std::string& input) const;
  QString setpoint_text(int output) const;
  QDoubleSpinBox* setpoint_field(int output) const;
  QPushButton* set_button(int output) const;
  // The last set or read failure, empty after a success.
  QString status_text() const;
  const StripChartModel& chart_model() const noexcept { return *model_; }
  StripChartView* chart() const noexcept { return chart_; }

 signals:
  // Every failed set or read, for the log.
  void cryoFailed(const QString& message);

 private:
  struct Loop {
    QDoubleSpinBox* field = nullptr;
    QPushButton* set = nullptr;
    QLabel* readback = nullptr;
  };

  void on_sample(const TemperatureSample& sample);
  void on_setpoint(int output, bool set, const Result<double>& setpoint);

  CoreBridge& bridge_;
  std::unique_ptr<StripChartModel> model_;
  StripChartView* chart_ = nullptr;
  std::map<std::string, QLabel*> temperatures_;
  std::map<int, Loop> loops_;
  QLabel* status_ = nullptr;
};

}  // namespace pychron::ui
