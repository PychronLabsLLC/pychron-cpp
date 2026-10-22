// PeakCenterWindow (spectrometer peak center design 5.1, 5.3) against the
// sim-integrated example spectrometer: the panel beside the plot, a run
// drawn as it goes, the close question while one runs, and the splitter and
// geometry kept in a temp ini file. The question is always a callback, so
// no modal dialog opens.

#include <filesystem>
#include <fstream>
#include <memory>

#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "peak_center_panel.hpp"
#include "peak_center_view.hpp"
#include "peak_center_window.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "settings_guard.hpp"
#include "spectrometer_bridge.hpp"
#include "spectrometer_fixture.hpp"

using namespace pychron;
using pychron::ui::PeakCenterWindow;
using pychron::ui::SpectrometerBridge;

namespace {

constexpr int kWaitMs = 10000;

}  // namespace

class TestPeakCenterWindow : public QObject {
  Q_OBJECT

 private:
  std::unique_ptr<QSettings> settings() const { return std::make_unique<QSettings>(path_, QSettings::IniFormat); }

  void open() {
    window_ = std::make_unique<PeakCenterWindow>(*bridge_, true, settings());
    // A peak_center.toml whose default set runs long enough to act on.
    const auto file = std::filesystem::path(dir_.filePath(QStringLiteral("pc.toml")).toStdString());
    std::ofstream(file) << "[default]\nisotope = \"Ar40\"\nwindow = 0.06\nstep = 0.0005\nintegration_s = 0.1\n"
                           "settle_s = 0.1\n";
    window_->set_peak_center_file(file);
  }
  void show() {
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
  }
  void start() {
    QTest::mouseClick(window_->findChild<QPushButton*>(QStringLiteral("peakCenterStart")), Qt::LeftButton);
    QVERIFY(window_->peak_center_running());
  }

  QTemporaryDir dir_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  QString path_;
  int file_ = 0;
  std::unique_ptr<VirtualClock> clock_;  // before the sim on it
  std::unique_ptr<ui::test::SimSpectrometer> sim_;
  std::unique_ptr<SpectrometerBridge> bridge_;
  std::unique_ptr<PeakCenterWindow> window_;

 private slots:
  void init() {
    path_ = dir_.filePath(QStringLiteral("settings-%1.ini").arg(file_++));
    sim_ = ui::test::make_sim_spectrometer();
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
  }

  void cleanup() {
    if (window_) window_->cancel_peak_center_and_wait();
    window_.reset();
    bridge_.reset();
    sim_.reset();
    clock_.reset();
  }

  void opensWithThePanelBesideThePlot() {
    open();
    show();
    QCOMPARE(window_->objectName(), QStringLiteral("PeakCenterWindow"));
    QCOMPARE(window_->windowTitle(), QStringLiteral("Peak Center (Simulation)"));
    auto* splitter = window_->findChild<QSplitter*>(QStringLiteral("peakCenterSplitter"));
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QCOMPARE(splitter->count(), 2);
    auto* scroll = qobject_cast<QScrollArea*>(splitter->widget(0));  // the form scrolls, the plot does not
    QVERIFY(scroll != nullptr);
    QCOMPARE(scroll->widget(), window_->peak_center_panel());
    QCOMPARE(splitter->widget(1), window_->peak_center_view());
    QVERIFY(window_->peak_center_panel()->isVisible());
    QVERIFY(window_->peak_center_view()->isVisible());
    QVERIFY(!window_->peak_center_running());
    QCOMPARE(window_->peak_center_panel()->file().filename(), std::filesystem::path("pc.toml"));
  }

  void aRunIsDrawnAsItGoes() {
    open();
    show();
    auto* view = window_->peak_center_view();
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    start();
    QCOMPARE(view->kept(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!view->series().empty() && view->series()[0].x.size() >= 2, kWaitMs);
    QVERIFY(view->title().contains(QStringLiteral("Ar40 on")));
    bridge_->cancel_peak_center();
    QVERIFY(finished.wait(kWaitMs));
    QVERIFY(!window_->peak_center_running());
    // A cancelled run has no result to keep; a failed fit or a centre would.
    QCOMPARE(view->kept(), 0);
  }

  // In simulated time with no participant here: the run takes no real time.
  void aFinishedRunIsKeptInThePlot() {
    bridge_.reset();
    sim_.reset();
    clock_ = std::make_unique<VirtualClock>();
    sim_ = ui::test::make_sim_spectrometer({}, clock_.get());
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    open();
    show();
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    start();
    QVERIFY(finished.wait(kWaitMs));
    QVERIFY(finished.at(0).at(0).value<Result<jobs::PeakCenterResult>>().has_value());
    QCOMPARE(window_->peak_center_view()->kept(), 1);
  }

  void closeDuringRunAsksAndNoKeepsItOpen() {
    open();
    QStringList asked;
    window_->set_ask([&](const QString& q) {
      asked << q;
      return false;
    });
    show();
    start();
    QVERIFY(!window_->close());
    QCOMPARE(asked, QStringList{QStringLiteral("Cancel peak center?")});
    QVERIFY(window_->isVisible());
    QVERIFY(window_->peak_center_running());
  }

  void yesCancelsAndClosesWhenTheRunHasFinished() {
    open();
    QStringList asked;
    window_->set_ask([&](const QString& q) {
      asked << q;
      return true;
    });
    show();
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    start();
    QVERIFY(!window_->close());  // not yet: the run ends through its own finish
    QCOMPARE(asked.size(), 1);
    QVERIFY(window_->isVisible());
    window_->close();  // a second close while waiting: ignored, not asked again
    QCOMPARE(asked.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!window_->isVisible(), kWaitMs);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(0).value<Result<jobs::PeakCenterResult>>().has_value());  // Cancelled
    QVERIFY(!window_->peak_center_running());
  }

  // The scan is the spectrometer window's: a cancel from here puts it back.
  void yesPutsTheScanBack() {
    open();
    window_->set_ask([](const QString&) { return true; });
    bridge_->start_scan(0.1);
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->status().running, kWaitMs);
    show();
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    start();
    window_->close();
    QVERIFY(finished.wait(kWaitMs));
    QTRY_VERIFY_WITH_TIMEOUT(!window_->isVisible(), kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->status().running, kWaitMs);
    bridge_->stop_scan();
    bridge_->drain();
  }

  void closeWithoutRunDoesNotAsk() {
    open();
    bool asked = false;
    window_->set_ask([&](const QString&) {
      asked = true;
      return false;
    });
    show();
    QVERIFY(window_->close());
    QVERIFY(!asked);
  }

  void cancelAndWaitEndsTheRunWithoutAsking() {
    open();
    int asked = 0;
    window_->set_ask([&](const QString&) {
      ++asked;
      return true;
    });
    show();
    start();
    window_->cancel_peak_center_and_wait();
    QVERIFY(!window_->peak_center_running());
    QVERIFY(window_->close());
    QCOMPARE(asked, 0);
  }

  void theSplitterAndGeometryComeBack() {
    open();
    show();
    // Inside the offscreen screen (800 x 600): a restore keeps a window on it.
    window_->resize(760, 520);
    auto* splitter = window_->findChild<QSplitter*>(QStringLiteral("peakCenterSplitter"));
    QTRY_COMPARE(window_->size(), QSize(760, 520));
    splitter->setSizes({splitter->width() - 400, 400});
    const QList<int> sizes = splitter->sizes();
    const QSize size = window_->size();
    QVERIFY(window_->close());
    QVERIFY(settings()->contains(QStringLiteral("peak_center_window/sim-integrated/splitter")));
    QVERIFY(settings()->contains(QStringLiteral("peak_center_window/sim-integrated/geometry")));
    window_.reset();

    open();
    show();
    splitter = window_->findChild<QSplitter*>(QStringLiteral("peakCenterSplitter"));
    QTRY_COMPARE(window_->size(), size);
    QVERIFY(window_->minimumSizeHint().height() < size.height());  // the form does not hold the window up
    constexpr int kSlackPx = 6;
    QTRY_VERIFY2(qAbs(splitter->sizes().at(0) - sizes.at(0)) <= kSlackPx,
                 qPrintable(QStringLiteral("%1 vs %2").arg(splitter->sizes().at(0)).arg(sizes.at(0))));
  }

  void corruptSettingsAreIgnored() {
    {
      auto s = settings();
      s->setValue(QStringLiteral("peak_center_window/sim-integrated/splitter"), QByteArray("not a state"));
      s->setValue(QStringLiteral("peak_center_window/sim-integrated/geometry"), QByteArray("nor a geometry"));
    }
    open();
    show();
    auto* splitter = window_->findChild<QSplitter*>(QStringLiteral("peakCenterSplitter"));
    QCOMPARE(splitter->count(), 2);
    QTRY_VERIFY(splitter->sizes().at(0) > 0 && splitter->sizes().at(1) > 0);
  }

  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(TestPeakCenterWindow)
#include "test_peak_center_window.moc"
