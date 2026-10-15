// What a gauge chip opens (gauge history design 4.2 and 4.3), fed from a
// ReadingHistory by hand: the hover popup.

#include <chrono>
#include <string>
#include <vector>

#include <QApplication>
#include <QGuiApplication>
#include <QLineEdit>
#include <QScreen>
#include <QVBoxLayout>
#include <QtTest/QtTest>

#include "gauge_popup.hpp"
#include "history_chart.hpp"
#include "reading_history.hpp"

using namespace std::chrono_literals;
using pychron::TimePoint;
using pychron::ui::GaugePopup;
using pychron::ui::ReadingHistory;

namespace {

const TimePoint kStart = TimePoint{} + 24h;

QDateTime wall_at(TimePoint ts) {
  return QDateTime(QDate(2026, 10, 14), QTime(8, 0, 0))
      .addMSecs(std::chrono::duration_cast<std::chrono::milliseconds>(ts - kStart).count());
}

// A main window, a history of IG1, and a popup over them; the line's time is `now`.
struct Fixture {
  QWidget window;
  QLineEdit* edit = new QLineEdit;
  ReadingHistory history{{"IG1"}, 3s};
  TimePoint now = kStart + 1h;
  int wall_calls = 0;
  GaugePopup popup{history, [this] { return now; },
                   [this](TimePoint ts) {
                     ++wall_calls;
                     return wall_at(ts);
                   },
                   &window};

  Fixture() {
    auto* layout = new QVBoxLayout(&window);
    layout->addWidget(edit);
    window.resize(400, 300);
    window.show();
  }

  void add(std::chrono::seconds ago, double value) { history.add("IG1", value, "torr", now - ago); }

  QRect chip() const { return QRect(100, 100, 60, 24); }
};

}  // namespace

class TestGaugeWindow : public QObject {
  Q_OBJECT

 private slots:
  void thePopupShowsTheLastTenMinutes() {
    Fixture f;
    f.add(20s, 1.1e-8);
    f.add(10s, 1.15e-8);
    f.add(0s, 1.2e-8);
    f.popup.show_for("IG1", f.chip());
    QVERIFY(f.popup.isVisible());
    QCOMPARE(f.popup.gauge(), QStringLiteral("IG1"));
    QCOMPARE(f.popup.heading(), QStringLiteral("IG1  1.20e-08 torr"));
    QVERIFY(f.popup.message().isEmpty());
    QVERIFY(f.popup.chart()->isVisible());
    QCOMPARE(f.popup.chart()->point_count(), 3);
  }

  void onlyTheLastTenMinutesAreDrawn() {
    Fixture f;
    f.add(15min, 1e-8);
    f.add(5min, 2e-8);
    f.add(1min, 3e-8);
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.chart()->point_count(), 2);
  }

  void theTenMinutesEndNowNotAtTheNewestReading() {
    Fixture f;
    f.add(14min, 1e-8);
    f.add(12min, 2e-8);
    f.add(8min, 3e-8);
    f.add(5min, 4e-8);
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.chart()->point_count(), 2);
    QCOMPARE(f.popup.heading(), QStringLiteral("IG1  4.00e-08 torr"));
  }

  void aTinyScreenStillHoldsThePopupsCorner() {
    const QRect at = GaugePopup::placed(QRect(10, 10, 20, 10), GaugePopup::kSize, QRect(0, 0, 100, 100));
    QCOMPARE(at.topLeft(), QPoint(0, 0));
    QCOMPARE(at.size(), GaugePopup::kSize);
  }

  void oneReadingIsNotAChart() {
    Fixture f;
    f.add(0s, 1e-8);
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.message(), QStringLiteral("No readings yet"));
    QVERIFY(!f.popup.chart()->isVisible());
    QCOMPARE(f.popup.heading(), QStringLiteral("IG1  1.00e-08 torr"));
  }

  void noReadingIsNotAChart() {
    Fixture f;
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.message(), QStringLiteral("No readings yet"));
    QCOMPARE(f.popup.heading(), QStringLiteral("IG1"));
  }

  void readingsAllOlderThanTenMinutesSaySo() {
    Fixture f;
    f.add(20min, 1e-8);
    f.add(15min, 2e-8);
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.message(), QStringLiteral("No readings in the last 10 minutes"));
    QVERIFY(!f.popup.chart()->isVisible());
  }

  void refreshAddsTheNewReading() {
    Fixture f;
    f.add(20s, 1e-8);
    f.add(10s, 2e-8);
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.chart()->point_count(), 2);
    f.now += 1s;
    f.add(0s, 3e-8);
    f.popup.refresh();
    QCOMPARE(f.popup.chart()->point_count(), 3);
    QCOMPARE(f.popup.heading(), QStringLiteral("IG1  3.00e-08 torr"));
  }

  void refreshWhenHiddenDoesNothing() {
    Fixture f;
    f.add(20s, 1e-8);
    f.add(10s, 2e-8);
    f.popup.show_for("IG1", f.chip());
    f.popup.hide();
    const int calls = f.wall_calls;
    f.add(0s, 3e-8);
    f.popup.refresh();
    QCOMPARE(f.popup.chart()->point_count(), 2);
    QCOMPARE(f.wall_calls, calls);
  }

  void theWallClockIsAskedOnceARefresh() {
    Fixture f;
    f.add(20s, 1e-8);
    f.add(10s, 2e-8);
    f.popup.show_for("IG1", f.chip());
    const int calls = f.wall_calls;
    f.popup.refresh();
    QCOMPARE(f.wall_calls, calls + 1);
  }

  void itIsASmallToolTipWindowThatTakesNoClick() {
    Fixture f;
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(f.popup.size(), GaugePopup::kSize);
    QCOMPARE(f.popup.windowType(), Qt::ToolTip);
    QVERIFY(f.popup.windowFlags().testFlag(Qt::WindowTransparentForInput));
    QVERIFY(f.popup.testAttribute(Qt::WA_TransparentForMouseEvents));
    QVERIFY(f.popup.testAttribute(Qt::WA_ShowWithoutActivating));
    QCOMPARE(f.popup.parentWidget(), &f.window);
  }

  void showingItTakesNoFocus() {
    Fixture f;
    f.window.activateWindow();
    f.edit->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(f.edit));
    f.add(20s, 1e-8);
    f.add(10s, 2e-8);
    f.popup.show_for("IG1", f.chip());
    QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(f.edit));
  }

  void itStaysBesideTheChipAndInsideTheScreen() {
    const QRect screen(0, 0, 1000, 800);
    const QSize size = GaugePopup::kSize;
    for (const QRect& chip : {QRect(100, 100, 60, 24), QRect(940, 100, 60, 24), QRect(940, 780, 60, 20),
                              QRect(100, 780, 60, 20), QRect(400, 0, 60, 24), QRect(0, 0, 1000, 24)}) {
      const QRect at = GaugePopup::placed(chip, size, screen);
      QCOMPARE(at.size(), size);
      QVERIFY2(screen.contains(at), qPrintable(QStringLiteral("chip at %1,%2").arg(chip.x()).arg(chip.y())));
      QVERIFY2(!at.intersects(chip), qPrintable(QStringLiteral("chip at %1,%2").arg(chip.x()).arg(chip.y())));
    }
    // Right of the chip when there is room, its top level with the chip's.
    const QRect right = GaugePopup::placed(QRect(100, 100, 60, 24), size, screen);
    QVERIFY(right.left() > 160);
    QCOMPARE(right.top(), 100);
    // Left of it at the right edge.
    QVERIFY(GaugePopup::placed(QRect(940, 100, 60, 24), size, screen).right() < 940);
  }

  void showForPlacesItOnTheChipsScreen() {
    Fixture f;
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    const QRect chip(screen.right() - 30, screen.bottom() - 20, 30, 20);
    f.popup.show_for("IG1", chip);
    QVERIFY(screen.contains(f.popup.geometry()));
    QVERIFY(!f.popup.geometry().intersects(chip));
  }

  void itHidesWhenTheApplicationIsDeactivated() {
    Fixture f;
    f.popup.show_for("IG1", f.chip());
    QVERIFY(f.popup.isVisible());
    emit qApp->applicationStateChanged(Qt::ApplicationInactive);
    QVERIFY(!f.popup.isVisible());
  }

  void itHidesWhenItsWindowIsDeactivated() {
    Fixture f;
    f.popup.show_for("IG1", f.chip());
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&f.window, &deactivate);
    QVERIFY(!f.popup.isVisible());
  }

  void itHidesWhenItsOwnerSaysSo() {
    Fixture f;
    f.popup.show_for("IG1", f.chip());
    f.popup.hide();
    QVERIFY(!f.popup.isVisible());
  }
};

QTEST_MAIN(TestGaugeWindow)
#include "test_gauge_window.moc"
