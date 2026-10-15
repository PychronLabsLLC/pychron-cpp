// What a gauge chip opens (gauge history design 4.2 and 4.3), fed from a
// ReadingHistory by hand: the hover popup and the gauge's window.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGuiApplication>
#include <QLineEdit>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest/QtTest>

#include "gauge_popup.hpp"
#include "gauge_window.hpp"
#include "history_chart.hpp"
#include "reading_history.hpp"

using namespace std::chrono_literals;
using pychron::TimePoint;
using pychron::ui::GaugePopup;
using pychron::ui::GaugeWindow;
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

// A history of IG1 and CG1, settings in a file of their own, and gauge windows over them.
struct WindowFixture {
  QTemporaryDir dir;
  ReadingHistory history{{"IG1", "CG1"}, 3s};
  TimePoint now = kStart + 2h;  // 10:00 on the wall
  int wall_calls = 0;

  GaugeWindow::Deps deps(bool wall = true) {
    GaugeWindow::Deps d;
    d.history = &history;
    if (wall) {
      d.wall_of = [this](TimePoint ts) {
        ++wall_calls;
        return wall_at(ts);
      };
    }
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    d.settings = [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); };
    d.line = QStringLiteral("Example");
    return d;
  }

  std::unique_ptr<GaugeWindow> open(const QString& name, GaugeWindow::Deps d) {
    return std::make_unique<GaugeWindow>(name, std::move(d));
  }
  std::unique_ptr<GaugeWindow> open(const QString& name) { return open(name, deps()); }

  void add(std::chrono::seconds ago, double value, const std::string& name = "IG1") {
    history.add(name, value, "torr", now - ago);
  }

  QSettings settings() { return QSettings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat); }
};

void choose(GaugeWindow& w, std::chrono::seconds range) {
  const int at = w.range_box()->findData(static_cast<qlonglong>(range.count()));
  QVERIFY(at >= 0);
  w.range_box()->setCurrentIndex(at);
}

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

  // The gauge window.

  void opensOnTenMinutesAndLinear() {
    WindowFixture f;
    const auto w = f.open("IG1");
    QCOMPARE(w->range(), 600s);
    QCOMPARE(w->range_box()->currentData().toLongLong(), 600);
    QCOMPARE(w->range_box()->count(), 5);
    QVERIFY(!w->log_box()->isChecked());
    QVERIFY(!w->chart()->log());
    QCOMPARE(GaugeWindow::ranges(), (std::vector<std::chrono::seconds>{600s, 1800s, 3600s, 21600s, 86400s}));
  }

  void theRangeChosenIsKeptForThatGauge() {
    WindowFixture f;
    {
      const auto ig1 = f.open("IG1");
      choose(*ig1, 1h);
      ig1->log_box()->setChecked(true);
      ig1->show();
      ig1->resize(700, 450);
      ig1->close();
    }
    QCOMPARE(f.settings().value("gauge_window/Example/IG1/range_s").toLongLong(), 3600);
    QVERIFY(f.settings().value("gauge_window/Example/IG1/log").toBool());
    QVERIFY(!f.settings().value("gauge_window/Example/IG1/geometry").toByteArray().isEmpty());

    const auto again = f.open("IG1");
    QCOMPARE(again->range(), 3600s);
    QVERIFY(again->log_box()->isChecked());
    QVERIFY(again->chart()->log());
    QCOMPARE(again->size(), QSize(700, 450));

    const auto cg1 = f.open("CG1");
    QCOMPARE(cg1->range(), 600s);
    QVERIFY(!cg1->log_box()->isChecked());

    // A range that is none of those offered is the default.
    f.settings().setValue("gauge_window/Example/CG1/range_s", 1234);
    QCOMPARE(f.open("CG1")->range(), 600s);
  }

  void withoutSettingsNothingIsKept() {
    WindowFixture f;
    auto d = f.deps();
    d.settings = nullptr;
    {
      const auto w = f.open("IG1", d);
      choose(*w, 1h);
      w->show();
      w->close();
    }
    QCOMPARE(f.open("IG1", d)->range(), 600s);
    QVERIFY(f.settings().allKeys().isEmpty());
  }

  void pointsOutsideTheRangeAreNotDrawn() {
    WindowFixture f;
    f.add(25min, 1e-8);
    f.add(15min, 2e-8);
    f.add(5min, 3e-8);
    f.add(0s, 4e-8);
    const auto w = f.open("IG1");
    QCOMPARE(w->chart()->point_count(), 2);
    choose(*w, 30min);
    QCOMPARE(w->chart()->point_count(), 4);
  }

  void theHeaderGivesLatestMinAndMaxOfTheRange() {
    WindowFixture f;
    f.add(15min, 9e-8);  // outside the ten minutes
    f.add(20s, 1.1e-8);
    f.add(10s, 1.4e-8);
    f.add(0s, 1.2e-8);
    const auto w = f.open("IG1");
    QCOMPARE(w->header(), QStringLiteral("1.20e-08 torr   min 1.10e-08   max 1.40e-08"));
  }

  void anEmptyHistorySaysSo() {
    WindowFixture f;
    const auto w = f.open("IG1");
    QCOMPARE(w->header(), QStringLiteral("No readings"));
    QCOMPARE(w->chart()->point_count(), 0);
  }

  void aRangeLongerThanTheHistorySaysSinceWhen() {
    WindowFixture f;
    for (int ago = 20 * 60; ago >= 0; ago -= 60) f.add(std::chrono::seconds(ago), 1e-8 + ago * 1e-12);
    const auto w = f.open("IG1");
    QVERIFY2(!w->header().contains("since"), qPrintable(w->header()));
    choose(*w, 1h);
    QVERIFY2(w->header().endsWith(QStringLiteral("   since 09:40")), qPrintable(w->header()));
  }

  void alarmLimitsAreGuides() {
    WindowFixture f;
    f.add(20s, 0.5e-8);
    f.add(10s, 3e-8);
    f.add(0s, 1.5e-8);
    auto d = f.deps();
    d.alarm_high = 2e-8;
    d.alarm_low = 1e-8;
    QCOMPARE(f.open("IG1", d)->chart()->guide_count(), 2);
    QCOMPARE(f.open("IG1")->chart()->guide_count(), 0);
  }

  void logAndLinear() {
    WindowFixture f;
    f.add(10s, 1e-9);
    f.add(0s, 1e-7);
    const auto w = f.open("IG1");
    w->log_box()->setChecked(true);
    QVERIFY(w->chart()->log());
    w->log_box()->setChecked(false);
    QVERIFY(!w->chart()->log());
  }

  void copyGivesTimeAndValue() {
    WindowFixture f;
    f.add(10s, 1.2345678901234567e-08);
    f.add(9s, 2e-8);
    f.add(8s, 3e-8);
    f.add(0s, 4e-8);  // 8 s after the last: a break before it
    const auto w = f.open("IG1");
    QCOMPARE(f.wall_calls, 1);
    const QString text = w->copy_text();
    QCOMPARE(f.wall_calls, 1);
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 5);
    QCOMPARE(lines[0], QStringLiteral("time\tvalue (torr)"));
    QCOMPARE(lines[1], QStringLiteral("2026-10-14 09:59:50\t1.2345678901234567e-08"));
    QCOMPARE(lines[2], QStringLiteral("2026-10-14 09:59:51\t2e-08"));
    QCOMPARE(lines[3], QStringLiteral("2026-10-14 09:59:52\t3e-08"));
    QCOMPARE(lines[4], QStringLiteral("2026-10-14 10:00:00\t4e-08"));
  }

  void aNewReadingIsDrawn() {
    WindowFixture f;
    f.add(20s, 1e-8);
    f.add(10s, 2e-8);
    const auto w = f.open("IG1");
    w->show();
    QCOMPARE(w->chart()->point_count(), 2);
    QTest::qWait(GaugeWindow::kMinRedrawMs + 50);  // the window has not drawn since

    // At once.
    const int before = w->redraw_count();
    f.now += 1s;
    f.add(0s, 3e-8);
    w->refresh();
    QCOMPARE(w->redraw_count(), before + 1);
    QCOMPARE(w->drawn_revision(), f.history.revision("IG1"));
    QCOMPARE(w->chart()->point_count(), 3);

    // Many in one turn of the event loop: once more, later.
    for (int i = 0; i < 5; ++i) {
      f.now += 1s;
      f.add(0s, 4e-8);
      w->refresh();
    }
    QCOMPARE(w->redraw_count(), before + 1);
    QTRY_COMPARE(w->redraw_count(), before + 2);
    QCOMPARE(w->drawn_revision(), f.history.revision("IG1"));
    QCOMPARE(w->chart()->point_count(), 8);
    QTest::qWait(GaugeWindow::kMinRedrawMs + 50);
    QCOMPARE(w->redraw_count(), before + 2);
  }

  void aHiddenWindowDrawsWhenShown() {
    WindowFixture f;
    f.add(20s, 1e-8);
    f.add(10s, 2e-8);
    const auto w = f.open("IG1");
    QVERIFY(!w->isVisible());
    const int before = w->redraw_count();
    f.add(0s, 3e-8);
    w->refresh();
    QCOMPARE(w->redraw_count(), before);
    QCOMPARE(w->chart()->point_count(), 2);
    w->show();
    QCOMPARE(w->redraw_count(), before + 1);
    QCOMPARE(w->chart()->point_count(), 3);
    QCOMPARE(w->drawn_revision(), f.history.revision("IG1"));
  }

  void theGeometryIsKeptWhenHidden() {
    WindowFixture f;
    {
      const auto w = f.open("IG1");
      w->show();
      w->resize(640, 420);
      w->hide();
    }
    QCOMPARE(f.open("IG1")->size(), QSize(640, 420));
  }

  void withoutAWallClockTheAxisIsMinutesBeforeNow() {
    WindowFixture f;
    f.add(10s, 1e-8);
    f.add(9s, 2e-8);
    f.add(8s, 3e-8);
    f.add(7s, 4e-8);
    const auto w = f.open("IG1", f.deps(false));
    QCOMPARE(w->chart()->shown_x().lo, -600.0);
    QCOMPARE(w->chart()->shown_x().hi, 0.0);
    const QStringList lines = w->copy_text().split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 5);
    QCOMPARE(lines[0], QStringLiteral("seconds before newest\tvalue (torr)"));
    QCOMPARE(lines[1], QStringLiteral("-3\t1e-08"));
    QCOMPARE(lines[4], QStringLiteral("0\t4e-08"));
    choose(*w, 1h);
    QVERIFY2(w->header().endsWith(QStringLiteral("   since 0.05 min before newest")), qPrintable(w->header()));
  }

  void theTitleIsTheGaugesName() {
    WindowFixture f;
    const auto w = f.open("IG1");
    QCOMPARE(w->windowTitle(), QStringLiteral("IG1"));
    QVERIFY(w->isWindow());
    QCOMPARE(w->gauge(), QStringLiteral("IG1"));
  }
};

QTEST_MAIN(TestGaugeWindow)
#include "test_gauge_window.moc"
