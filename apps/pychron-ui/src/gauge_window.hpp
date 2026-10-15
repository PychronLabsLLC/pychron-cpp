#pragma once

// GaugeWindow (gauge history design 4.3): what clicking a gauge chip opens, a
// window of its own for that gauge. A chart of the gauge's readings over a
// range the operator chooses (10 minutes to a day), linear or log, with the
// gauge's alarm limits drawn as guides when they fall inside the view; a
// header with the latest reading, the lowest and highest of the range, and
// since when the history holds readings when the range is longer than that;
// and Copy, which puts the points shown on the clipboard as text.
//
// The range, the scale and the window's geometry are kept per gauge in the
// settings (`gauge_window/<line>/<gauge>/...`). The window reads the history
// and never a clock of its own: a wall time comes from `wall_of`, the line's.
// New readings are drawn at most four times a second, and not while the
// window is hidden: it catches up when shown.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QDateTime>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include "pychron/core/clock.hpp"
#include "reading_history.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QSettings;

namespace pychron::ui {

class HistoryChart;

class GaugeWindow : public QWidget {
  Q_OBJECT

 public:
  static constexpr int kMinRedrawMs = 250;  // redraw cap: 4 a second
  static constexpr std::chrono::seconds kDefaultRange{600};

  struct Deps {
    const ReadingHistory* history = nullptr;
    std::function<QDateTime(TimePoint)> wall_of;            // null: no wall labels
    std::optional<double> alarm_high, alarm_low;
    std::function<std::unique_ptr<QSettings>()> settings;  // null: nothing kept; opened for each read and write
    QString line;                                           // settings key
  };

  GaugeWindow(QString name, Deps deps, QWidget* parent = nullptr);

  void refresh();  // new reading: at most 4 redraws a second; when hidden, on the next show
  static const std::vector<std::chrono::seconds>& ranges();  // 600, 1800, 3600, 21600, 86400

  [[nodiscard]] const QString& gauge() const { return name_; }
  [[nodiscard]] std::chrono::seconds range() const;

  // For tests.
  [[nodiscard]] QComboBox* range_box() const { return range_box_; }
  [[nodiscard]] QCheckBox* log_box() const { return log_box_; }
  [[nodiscard]] HistoryChart* chart() const { return chart_; }
  [[nodiscard]] QString header() const;  // "1.20e-08 torr   min 1.10e-08   max 1.40e-08   since 14:02"
  [[nodiscard]] QString copy_text() const;
  [[nodiscard]] int redraw_count() const { return redraws_; }
  [[nodiscard]] unsigned drawn_revision() const { return drawn_revision_; }

 protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  void draw();
  void update_header();
  void save(const QString& leaf, const QVariant& value) const;
  [[nodiscard]] QString key(const QString& leaf) const;

  QString name_;
  Deps deps_;
  QLabel* header_;
  QComboBox* range_box_;
  QCheckBox* log_box_;
  HistoryChart* chart_;

  ReadingHistory::Series shown_;  // what was last drawn
  QDateTime newest_wall_;         // the wall time of shown_'s newest reading; invalid without a wall clock
  QElapsedTimer since_draw_;
  QTimer deferred_;  // single shot: the redraw a too-early refresh() asked for
  int redraws_ = 0;
  unsigned drawn_revision_ = 0;
  bool stale_ = false;  // a reading came while hidden
};

}  // namespace pychron::ui
