#pragma once

// GaugePopup (gauge history design 4.2): what resting the pointer on a gauge
// chip shows, beside the chip: the gauge's name, its latest reading and a
// compact chart of its last ten minutes, kept current while shown. Fewer
// than two readings in those ten minutes give a line of text instead of an
// empty chart.
//
// A Qt::ToolTip window, transparent to the mouse and shown without being
// activated: it takes no focus and no click meant for the canvas (a
// Qt::Popup would take the click that closes it). Parented to the main
// window, which hides it; it hides itself when the application or that
// window is deactivated. It reads the history and never a clock of its own:
// `now` and `wall_of` are the line's (CoreBridge).

#include <chrono>
#include <functional>

#include <QDateTime>
#include <QFrame>
#include <QRect>
#include <QString>

#include "pychron/core/clock.hpp"
#include "reading_history.hpp"

class QLabel;
class QStackedWidget;

namespace pychron::ui {

class HistoryChart;

class GaugePopup : public QFrame {
  Q_OBJECT

 public:
  static constexpr std::chrono::seconds kRange{600};
  static constexpr QSize kSize{320, 180};

  // `now`: the line's time, to tell how old the readings are. `wall_of`: the
  // wall time of a reading's ts, for the chart's axis. `parent`: the main window.
  GaugePopup(const ReadingHistory& history, std::function<TimePoint()> now,
             std::function<QDateTime(TimePoint)> wall_of, QWidget* parent);

  void show_for(const QString& name, const QRect& global_chip);  // beside the chip, clear of it, inside the screen
  void refresh();                                                 // that gauge has a new reading; nothing when hidden
  [[nodiscard]] const QString& gauge() const;
  [[nodiscard]] QString heading() const;  // "IG1  1.20e-08 torr"
  [[nodiscard]] QString message() const;  // empty when the chart is shown
  [[nodiscard]] HistoryChart* chart() const;

  // Where a popup of `size` goes for a chip at `chip` on a screen of `screen`:
  // right of the chip, else left of it, else below or above; inside the screen.
  [[nodiscard]] static QRect placed(const QRect& chip, const QSize& size, const QRect& screen);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void update_contents();

  const ReadingHistory& history_;
  std::function<TimePoint()> now_;
  std::function<QDateTime(TimePoint)> wall_of_;
  QString gauge_;
  QLabel* heading_;
  QStackedWidget* stack_;
  HistoryChart* chart_;
  QLabel* message_;
};

}  // namespace pychron::ui
