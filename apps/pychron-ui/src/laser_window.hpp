#pragma once

// LaserWindow (laser window design, sections 1 and 6): one extraction
// device's tray, camera, stage, laser, calibration and patterns, over a
// LaserBridge. It draws what the bridge publishes and turns clicks into the
// bridge's commands; it never calls the core.
//
// While a queue drives the lasers the window only watches: every control but
// the emergency stop is disabled. After an emergency stop nothing drives
// until Reset.

#include <functional>

#include <QMainWindow>
#include <QString>

#include "laser_bridge.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class CameraView;
class TrayView;

class LaserWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `bridge` must outlive the window.
  LaserWindow(LaserBridge& bridge, bool simulation, QWidget* parent = nullptr);

  // Shows a "Pattern maker..." button that calls `open`.
  void set_pattern_maker(std::function<void()> open);
  // The lab's patterns, read again (one was saved).
  void refresh_patterns();

  TrayView* tray_view() const noexcept { return tray_; }

 protected:
  // A beam opened here is closed when the window goes: there would be
  // nothing on screen to close it with.
  void closeEvent(QCloseEvent* event) override;

 public:
  CameraView* camera_view() const noexcept { return camera_; }

 private:
  void build();
  QWidget* build_control();
  QWidget* build_calibration();
  QWidget* build_patterns();
  void on_snapshot(const laser::LaserSnapshot& state);
  void on_finished(const QString& what, const Result<void>& result);
  void refresh_tray();         // the tray view, the calibration tab: from disk
  void refresh_enabled();
  void jog(double dx, double dy, double dz);
  QString selected_pattern() const;

  LaserBridge& bridge_;
  bool simulation_;
  QString last_snapshot_;
  QString shown_tray_;  // what the tray view and the calibration tab show
  std::function<void()> open_pattern_maker_;

  TrayView* tray_ = nullptr;
  CameraView* camera_ = nullptr;
  QComboBox* tray_choice_ = nullptr;
  QLabel* banner_ = nullptr;
  QPushButton* estop_ = nullptr;
  QPushButton* reset_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* activity_ = nullptr;
  QLabel* position_ = nullptr;
  QDoubleSpinBox* step_ = nullptr;
  QList<QPushButton*> jogs_;
  QPushButton* stop_stage_ = nullptr;
  QCheckBox* centre_ = nullptr;
  QPushButton* autocenter_ = nullptr;
  QLabel* outcome_ = nullptr;
  QPushButton* snapshot_ = nullptr;
  QPushButton* measure_scale_ = nullptr;
  QLabel* camera_scale_ = nullptr;
  QPushButton* enable_ = nullptr;
  QDoubleSpinBox* output_ = nullptr;
  QPushButton* fire_ = nullptr;
  QPushButton* stop_beam_ = nullptr;
  QLabel* beam_ = nullptr;
  QLabel* interlocks_ = nullptr;
  QTableWidget* cal_table_ = nullptr;
  QComboBox* cal_hole_ = nullptr;
  QPushButton* cal_add_ = nullptr;
  QPushButton* cal_remove_ = nullptr;
  QPushButton* cal_clear_ = nullptr;
  QLabel* cal_solution_ = nullptr;
  QLabel* cal_cautions_ = nullptr;
  QTableWidget* pattern_list_ = nullptr;
  QPushButton* pattern_run_ = nullptr;
  QPushButton* pattern_stop_ = nullptr;
  QPushButton* pattern_maker_ = nullptr;
};

}  // namespace pychron::ui
