#pragma once

// RiseRatePanel: the "Rise rate" group of the spectrometer window's controls.
// The operator picks a detector and presses Start; the panel takes that
// detector's value from every reading it is handed, shows the rate so far,
// and on Stop adds the result to a table kept for the session. It sends
// nothing to the instrument and reads no clock: a duration is the difference
// of two readings' times, and a result's wall time comes from the function it
// was given, or is blank.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QDateTime>
#include <QWidget>

#include "rise_rate.hpp"
#include "strip_chart_model.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class RiseRatePanel : public QWidget {
  Q_OBJECT

 public:
  enum Column { ColNumber, ColDetector, ColEndpoints, ColLinear, ColError, ColUnits, ColMinutes, ColReadings, ColTime, ColNote, ColCount };

  explicit RiseRatePanel(std::vector<DetectorSeries> detectors, QWidget* parent = nullptr);

  void on_reading(const spectrometer::IntensityReading& reading);
  // Ignored while a run is on, and for a detector the panel does not have.
  void set_detector(const QString& name);
  // The isotope on a detector, named in the results beside it.
  void set_isotope(const std::string& detector, const QString& isotope);
  void set_wall_clock(std::function<QDateTime()> now);
  void toggle();  // the Start / Stop button
  [[nodiscard]] bool running() const noexcept { return run_.running(); }

  // For tests.
  [[nodiscard]] QTableWidget* results() const noexcept { return results_; }
  [[nodiscard]] QComboBox* detector_box() const noexcept { return detector_; }
  [[nodiscard]] QPushButton* toggle_button() const noexcept { return toggle_; }
  [[nodiscard]] QPushButton* clear_button() const noexcept { return clear_; }
  [[nodiscard]] QString live_text() const;
  [[nodiscard]] QString status_text() const;
  // What Copy puts on the clipboard: a header line and a line a result, tab
  // separated.
  [[nodiscard]] QString copy_text() const;

 signals:
  void started(pychron::TimePoint at);  // the run's first reading
  void stopped(pychron::TimePoint at);  // its last; only when it gave a result
  void cleared();                       // a run began, or ended with no result: its marks go

 private:
  void show_live();
  void add_result(const RiseRate& rate);
  [[nodiscard]] QString units_of(const std::string& detector) const;

  std::vector<DetectorSeries> detectors_;
  std::map<std::string, QString> isotopes_;
  std::function<QDateTime()> wall_now_;
  RiseRun run_;
  bool announced_ = false;  // started() sent for this run
  int count_ = 0;           // results since the table was last cleared

  QComboBox* detector_;
  QPushButton* toggle_;
  QLabel* live_;
  QLabel* status_;
  QTableWidget* results_;
  QPushButton* copy_;
  QPushButton* clear_;
};

}  // namespace pychron::ui
