// SpectrometerWindow and StripChartView against the sim-integrated example
// spectrometer: the scan follows the window, the chart follows the model, the
// magnet controls move only on Apply (asking first for a large move), errors
// reach the banner, and settings survive a close. Settings live in a temp ini
// file that seeds the integration at 0.1 s (the sim's quantum); the move
// confirmation is always a callback, so no modal dialog opens.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "plot_style.hpp"
#include "preferences.hpp"
#include "spectrometer_bridge.hpp"
#include "spectrometer_fixture.hpp"
#include "settings_guard.hpp"
#include "spectrometer_window.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using pychron::spectrometer::IntensityReading;
using pychron::spectrometer::MagnetMoved;
using pychron::ui::AxisRange;
using pychron::ui::IntensitiesModel;
using pychron::ui::SpectrometerBridge;
using pychron::ui::SpectrometerWindow;
using pychron::ui::YScale;

namespace {

constexpr int kWaitMs = 10000;
constexpr int kH1 = 1;  // config order: H2, H1, AX, L1, L2, CDD
constexpr int kAX = 2;

// A row at `seconds` giving every listed detector the same value.
IntensityReading row(double seconds, double value, std::initializer_list<const char*> detectors = {"H1", "AX"}) {
  IntensityReading r;
  r.reading.ts = TimePoint{} + std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds));
  for (const char* detector : detectors) {
    r.reading.values[detector] = spectrometer::Value{value, std::nullopt, std::nullopt, false};
  }
  return r;
}

}  // namespace

class TestSpectrometerWindow : public QObject {
  Q_OBJECT

 private:
  static QString key(const char* name) { return QStringLiteral("spectrometer_window/sim-integrated/%1").arg(name); }
  std::unique_ptr<QSettings> settings() const { return std::make_unique<QSettings>(path_, QSettings::IniFormat); }
  void seed(const char* name, const QVariant& value) { settings()->setValue(key(name), value); }
  QVariant saved(const char* name) const { return settings()->value(key(name)); }

  // A window on the temp settings file whose large-move question answers yes.
  void open() {
    window_ = std::make_unique<SpectrometerWindow>(*bridge_, true, settings());
    window_->set_confirm_move([](double) { return true; });
  }
  // Counts the questions asked from here on, answering `answer`.
  void answer_moves(bool answer) {
    asked_.clear();
    window_->set_confirm_move([this, answer](double delta) {
      asked_.push_back(delta);
      return answer;
    });
  }
  QString isotope_shown(int detector_row) const {
    return window_->intensities()->index(detector_row, IntensitiesModel::ColIsotope).data().toString();
  }
  // A peak_center.toml whose default set runs long enough to act on.
  void write_pc_file() {
    const auto file = std::filesystem::path(dir_.filePath(QStringLiteral("pc.toml")).toStdString());
    std::ofstream(file) << "[default]\nisotope = \"Ar40\"\nwindow = 0.06\nstep = 0.0005\nintegration_s = 0.1\n"
                           "settle_s = 0.1\n";
    window_->set_peak_center_file(file);
  }
  // Applies the selected target and waits for the move to finish.
  void apply_and_wait() {
    const int before = positions_;
    window_->apply_position();
    QVERIFY(!window_->apply_enabled());  // pending
    QTRY_COMPARE_WITH_TIMEOUT(positions_, before + 1, kWaitMs);
    QVERIFY(window_->apply_enabled());
  }

  QTemporaryDir dir_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  QString path_;
  int file_ = 0;
  std::unique_ptr<ui::test::SimSpectrometer> sim_;
  std::unique_ptr<SpectrometerBridge> bridge_;
  std::unique_ptr<SpectrometerWindow> window_;
  int moves_ = 0;      // magnetMoved
  int positions_ = 0;  // commandFinished("position")
  std::vector<double> asked_;

 private slots:
  void init() {
    path_ = dir_.filePath(QStringLiteral("settings-%1.ini").arg(file_++));
    seed("integration_s", 0.1);
    sim_ = ui::test::make_sim_spectrometer();
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    connect(bridge_.get(), &SpectrometerBridge::magnetMoved, this, [this](const MagnetMoved&) { ++moves_; });
    connect(bridge_.get(), &SpectrometerBridge::commandFinished, this, [this](const QString& what, const Result<void>&) {
      positions_ += what == QLatin1String("position") ? 1 : 0;
    });
    moves_ = 0;
    positions_ = 0;
    asked_.clear();
  }

  void cleanup() {
    ui::set_plot_style(ui::default_plot_style());
    window_.reset();
    bridge_.reset();
    sim_.reset();
  }

  void a_config_color_survives_a_palette_change() {
    // H1 keeps a colour of its own; AX has none, so it takes palette slot 2.
    bridge_.reset();
    sim_ = ui::test::make_sim_spectrometer([](spectrometer::cfg::SpectrometerData& data) {
      data.config.detectors[std::size_t{kH1}].color = "#123456";
      data.config.detectors[std::size_t{kAX}].color.clear();
    });
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    open();
    const auto pen = [this](int d) { return window_->chart_view()->plot()->graph(d)->pen().color().name(); };
    const auto swatch = [this](int d) {
      return window_->intensities()->index(d, IntensitiesModel::ColColour).data(Qt::BackgroundRole).value<QBrush>().color().name();
    };
    QCOMPARE(pen(kAX), ui::plot_series_color(std::size_t{kAX}).name());

    ui::Preferences preferences;
    preferences.plot_background = "#fafad2";
    for (int slot = 1; slot <= ui::Preferences::kPlotSeriesSlots; ++slot)
      preferences.plot_series_colors[slot] = QColor::fromHsv(slot * 25, 200, 150).name().toStdString();
    ui::apply_application_preferences(preferences);

    const QString slot_color = QColor::fromHsv((kAX + 1) * 25, 200, 150).name();
    QCOMPARE(pen(kH1), QStringLiteral("#123456"));
    QCOMPARE(swatch(kH1), QStringLiteral("#123456"));
    QCOMPARE(pen(kAX), slot_color);
    QCOMPARE(swatch(kAX), slot_color);
    QCOMPARE(window_->chart_view()->plot()->axisRect()->backgroundBrush().color().name(), QStringLiteral("#fafad2"));
  }

  void openingStartsScanAndLinesGainPoints() {
    open();
    QVERIFY(window_->windowTitle().endsWith(QStringLiteral("(Simulation)")));
    QCOMPARE(window_->chart_view()->graph_count(), static_cast<int>(sim_->spec->config().detectors.size()));
    QVERIFY(!sim_->scan->running());  // nothing scans until the window is shown
    QCOMPARE(window_->chart_view()->point_count(0), 0);

    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(0) >= 3, kWaitMs);  // drawn by the window's timer
    QVERIFY(sim_->scan->running());
    QCOMPARE(sim_->scan->integration(), Duration(100ms));  // the saved integration
    // The magnet position was read when the scan started.
    QVERIFY(!window_->position_text().isEmpty());

    SpectrometerWindow real(*bridge_, false, settings());
    QCOMPARE(real.windowTitle(), QStringLiteral("Spectrometer"));
  }

  // The detector list is the Intensities table: a double click on a row
  // hides or shows that detector's line; the row greys and goes on updating.
  void doubleClickingADetectorRowTogglesItsLine() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(kAX) >= 2, kWaitMs);
    QTableView* table = window_->intensities_view();
    QVERIFY(table);
    const QModelIndex cell = table->model()->index(kAX, IntensitiesModel::ColIntensity);
    const auto double_click = [&] {
      table->scrollTo(cell);
      // As a hand does it: the view takes a double click only on the cell it was pressed on.
      QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(cell).center());
      QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(cell).center());
      window_->chart_view()->refresh();
    };

    double_click();
    QVERIFY(!window_->chart_view()->graph_visible(kAX));
    QVERIFY(window_->chart_view()->graph_visible(kH1));
    QVERIFY(!window_->intensities()->shown(kAX));
    QVERIFY(window_->intensities()->shown(kH1));
    // Off, and its number still moves.
    const int before = window_->chart_view()->point_count(kAX);
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(kAX) > before, kWaitMs);
    QVERIFY(table->model()->data(cell).toString() != QStringLiteral("\u2014"));

    double_click();
    QVERIFY(window_->chart_view()->graph_visible(kAX));
    QVERIFY(window_->intensities()->shown(kAX));

    // What the window is told by name (its saved settings) reaches the table too.
    window_->set_detector_shown("AX", false);
    QVERIFY(!window_->intensities()->shown(kAX));
    window_->set_detector_shown("AX", true);
    QVERIFY(window_->intensities()->shown(kAX));
  }

  void theControlsHaveNoDetectorList() {
    open();
    for (const auto* box : window_->findChildren<QGroupBox*>()) QVERIFY(box->title() != QStringLiteral("Detectors"));
  }

  void uncheckingDetectorHidesGraphButKeepsBuffering() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(kAX) >= 2, kWaitMs);
    QVERIFY(window_->chart_view()->graph_visible(kAX));

    window_->set_detector_shown("AX", false);
    window_->set_detector_shown("ZZ", false);  // unknown: ignored
    window_->chart_view()->refresh();
    QVERIFY(!window_->chart_view()->graph_visible(kAX));
    QVERIFY(window_->chart_view()->graph_visible(kH1));
    const int before = window_->chart_view()->point_count(kAX);
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(kAX) > before, kWaitMs);
    QVERIFY(!window_->chart_view()->graph_visible(kAX));

    window_->set_detector_shown("AX", true);
    window_->chart_view()->refresh();
    QVERIFY(window_->chart_view()->graph_visible(kAX));
  }

  // Typing "15" passes through "1": the width (and with it the history the
  // ring keeps) must not change until the edit is committed.
  void typedScanWidthAppliesOnCommitOnly() {
    open();
    // The Controls dock's: the Peak center dock has spin boxes of its own.
    auto* controls = window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerControlsDock"));
    QVERIFY(controls != nullptr);
    auto* spin = controls->findChild<QDoubleSpinBox*>();
    QVERIFY(spin != nullptr);
    QCOMPARE(window_->chart_model().scan_width(), 60.0);
    spin->selectAll();
    QTest::keyClicks(spin, QStringLiteral("15"));
    QCOMPARE(window_->chart_model().scan_width(), 60.0);
    QTest::keyClick(spin, Qt::Key_Return);
    QCOMPARE(window_->chart_model().scan_width(), 900.0);
  }

  void scanWidthAndRangesReachThePlot() {
    open();  // not shown: the chart is fed by hand
    auto* view = window_->chart_view();
    window_->set_scan_width_minutes(2.0);
    QCOMPARE(window_->chart_model().scan_width(), 120.0);
    view->refresh();
    QCOMPARE(view->shown_x().lo, 0.0);
    QCOMPARE(view->shown_x().hi, 126.0);

    window_->chart_model().append(row(0.0, 10.0));
    window_->chart_model().append(row(200.0, 20.0));
    view->refresh();
    const AxisRange x = window_->chart_model().x_range();
    QCOMPARE(x.lo, 80.0);
    QCOMPARE(view->shown_x().lo, x.lo);
    QCOMPARE(view->shown_x().hi, x.hi);
    const AxisRange y = window_->chart_model().y_range(std::chrono::steady_clock::now());
    QCOMPARE(view->shown_y().lo, y.lo);
    QCOMPARE(view->shown_y().hi, y.hi);

    // Manual limits reach the plot too; an inverted pair is rejected.
    window_->set_autoscale(false);
    QVERIFY(window_->set_manual_y(-5.0, 50.0));
    QVERIFY(!window_->set_manual_y(7.0, 3.0));
    view->refresh();
    QCOMPARE(view->shown_y().lo, -5.0);
    QCOMPARE(view->shown_y().hi, 50.0);
  }

  void logScaleWithNonPositiveDataDoesNotBreakAxis() {
    open();
    auto* view = window_->chart_view();
    auto usable = [view] {
      const AxisRange y = view->shown_y();
      return std::isfinite(y.lo) && std::isfinite(y.hi) && y.lo > 0.0 && y.lo < y.hi;
    };
    // Baseline noise only: nothing positive to plot.
    window_->chart_model().append(row(0.0, -1.0));
    window_->chart_model().append(row(1.0, 0.0));
    view->refresh();
    window_->set_scale(YScale::Log);
    view->refresh();
    QVERIFY(usable());
    QCOMPARE(view->point_count(kH1), 2);

    window_->clear_chart();  // lets the autoscale recompute at once
    window_->chart_model().append(row(0.0, -3.0));
    window_->chart_model().append(row(1.0, 2.0));
    window_->chart_model().append(row(2.0, 50.0));
    view->refresh();
    QVERIFY(usable());
    QVERIFY(view->shown_y().lo <= 2.0);
    QVERIFY(view->shown_y().hi >= 50.0);

    // A manual range that is not valid on a log axis is refused.
    window_->set_autoscale(false);
    QVERIFY(!window_->set_manual_y(-1.0, 10.0));
    view->refresh();
    QVERIFY(usable());

    window_->set_scale(YScale::Linear);
    view->refresh();
    QVERIFY(std::isfinite(view->shown_y().lo) && view->shown_y().lo < view->shown_y().hi);
  }

  void theControlsHaveARiseRateGroup() {
    open();
    bool found = false;
    for (const auto* box : window_->findChildren<QGroupBox*>()) found = found || box->title() == QStringLiteral("Rise rate");
    QVERIFY(found);
    QVERIFY(window_->rise_rate() != nullptr);
    // It starts on the detector the magnet group names, and follows it.
    QCOMPARE(window_->rise_rate()->detector_box()->currentText(), bridge_->reference_detector());
    window_->select_target("AX", "Ar40");
    QCOMPARE(window_->rise_rate()->detector_box()->currentText(), QStringLiteral("AX"));
  }

  void aRiseRateRunOnTheSimulatedSpectrometer() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(kH1) >= 2, kWaitMs);
    ui::RiseRatePanel* rise = window_->rise_rate();
    rise->set_detector(QStringLiteral("H1"));
    QCOMPARE(window_->chart_view()->mark_count(), 0);

    rise->toggle();
    QTRY_COMPARE_WITH_TIMEOUT(window_->chart_view()->mark_count(), 1, kWaitMs);  // the start, at its first reading
    const int before = window_->chart_view()->point_count(kH1);
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(kH1) >= before + 5, kWaitMs);
    QVERIFY(rise->live_text().contains(QStringLiteral("readings")));
    rise->toggle();
    QCOMPARE(rise->results()->rowCount(), 1);
    QCOMPARE(window_->chart_view()->mark_count(), 2);
    QVERIFY(rise->results()->item(0, ui::RiseRatePanel::ColDetector)->text().startsWith(QStringLiteral("H1")));
    QVERIFY(rise->results()->item(0, ui::RiseRatePanel::ColReadings)->text().toInt() >= 5);
    QVERIFY(rise->results()->item(0, ui::RiseRatePanel::ColTime)->text().isEmpty());  // this window was given no clock

    // A new run takes the last one's marks away at once.
    rise->toggle();
    QVERIFY(window_->chart_view()->mark_count() <= 1);
    QTRY_COMPARE_WITH_TIMEOUT(window_->chart_view()->mark_count(), 1, kWaitMs);
    // And clearing the chart takes its own: their place was in the chart's time.
    window_->clear_chart();
    QCOMPARE(window_->chart_view()->mark_count(), 0);
    // The run itself goes on over a cleared chart. Its start is not on the
    // new chart, so its stop is not drawn alone.
    QTRY_VERIFY_WITH_TIMEOUT(rise->live_text().contains(QStringLiteral("readings")), kWaitMs);
    rise->toggle();
    QCOMPARE(rise->results()->rowCount(), 2);
    QCOMPARE(window_->chart_view()->mark_count(), 0);
  }

  // The chart begins again by itself when a reading is earlier than the last:
  // marks of a finished run would be left at places that mean nothing.
  void marksGoWhenTheChartResetsItself() {
    open();  // not shown: fed by hand
    ui::RiseRatePanel* rise = window_->rise_rate();
    rise->set_detector(QStringLiteral("H1"));
    rise->toggle();
    emit bridge_->readings({row(100.0, 1.0), row(160.0, 2.0), row(220.0, 3.0)});
    rise->toggle();
    QCOMPARE(rise->results()->rowCount(), 1);
    QCOMPARE(window_->chart_view()->mark_count(), 2);
    emit bridge_->readings({row(5.0, 1.0)});
    QCOMPARE(window_->chart_view()->mark_count(), 0);
  }

  void closingTheWindowAbandonsARun() {
    open();
    window_->show();
    ui::RiseRatePanel* rise = window_->rise_rate();
    rise->toggle();
    QTRY_VERIFY_WITH_TIMEOUT(rise->live_text().contains(QStringLiteral("readings")), kWaitMs);
    QVERIFY(window_->close());
    QVERIFY(!rise->running());
    QCOMPARE(rise->results()->rowCount(), 0);
    QCOMPARE(window_->chart_view()->mark_count(), 0);
    QVERIFY(rise->status_text().contains(QStringLiteral("closed")));
  }

  void restartingTheScanAbandonsARun() {
    open();
    window_->show();
    ui::RiseRatePanel* rise = window_->rise_rate();
    rise->toggle();
    QTRY_VERIFY_WITH_TIMEOUT(rise->live_text().contains(QStringLiteral("readings")), kWaitMs);
    window_->restart_scan();
    QVERIFY(!rise->running());
    QCOMPARE(rise->results()->rowCount(), 0);
    QVERIFY(rise->status_text().contains(QStringLiteral("restarted")));
  }

  void aResultIsStampedWithTheTimeTheWindowIsGiven() {
    open();
    window_->set_wall_clock([] { return QDateTime(QDate(2026, 10, 13), QTime(1, 2, 3)); });
    window_->show();
    ui::RiseRatePanel* rise = window_->rise_rate();
    rise->toggle();
    QTRY_VERIFY_WITH_TIMEOUT(rise->live_text().contains(QStringLiteral("readings")), kWaitMs);
    rise->toggle();
    QCOMPARE(rise->results()->item(0, ui::RiseRatePanel::ColTime)->text(), QStringLiteral("2026-10-13 01:02:03"));
  }

  void aResultGoesToTheFileTheWindowIsGiven() {
    open();
    const QString path = dir_.filePath(QStringLiteral("rise_rates.csv"));
    QFile::remove(path);
    window_->set_rise_rate_file(path.toStdString());
    window_->show();
    ui::RiseRatePanel* rise = window_->rise_rate();
    rise->toggle();
    QTRY_VERIFY_WITH_TIMEOUT(rise->live_text().contains(QStringLiteral("readings")), kWaitMs);
    rise->toggle();
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 2);
    // This window's spectrometer, and that it is a simulated one.
    QVERIFY2(lines[1].contains(QStringLiteral(",sim-integrated,yes,")), qPrintable(lines[1]));
  }

  void marksAreDrawnAndReplaced() {
    ui::StripChartModel model(bridge_->detectors());
    ui::StripChartView view(model);
    QCOMPARE(view.mark_count(), 0);
    view.set_marks({{1.0, QColor(Qt::black)}, {2.0, QColor(Qt::red)}});
    QCOMPARE(view.mark_count(), 2);
    view.set_marks({{3.0, QColor(Qt::black)}});
    QCOMPARE(view.mark_count(), 1);
    model.set_scale(ui::YScale::Log);  // a mark does not mind the axis
    view.refresh();
    QCOMPARE(view.mark_count(), 1);
    view.set_marks({});
    QCOMPARE(view.mark_count(), 0);
  }

  // A minute of 0.1 s readings on every trace: the repaint runs on the GUI
  // thread, so one that takes long freezes the window.
  void repaintOfFullChartStaysShort() {
    ui::StripChartModel model(bridge_->detectors());
    ui::StripChartView view(model);
    view.resize(800, 500);
    view.show();  // the plot gets its size from the layout
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    for (int i = 0; i < 600; ++i) {
      const double noise = (i * 7919 % 100) / 100.0;  // zigzag, like detector noise
      model.append(row(0.1 * i, 10.0 + noise, {"H2", "H1", "AX", "L1", "L2", "CDD"}));
    }
    QTest::qWait(600);  // past the repaint cap and the autoscale interval: refresh() rescales and draws at once
    QElapsedTimer timer;
    timer.start();
    view.refresh();
    const qint64 elapsed = timer.elapsed();
    QCOMPARE(view.point_count(kH1), 600);
    QVERIFY2(elapsed < 250, qPrintable(QStringLiteral("repaint took %1 ms").arg(elapsed)));
  }

  void clearEmptiesChart() {
    open();
    auto* view = window_->chart_view();
    window_->chart_model().append(row(0.0, 1.0));
    window_->chart_model().append(row(1.0, 2.0, {"H1"}));  // AX missed this row: a gap, still a point
    view->refresh();
    QCOMPARE(view->point_count(kH1), 2);
    QCOMPARE(view->point_count(kAX), 1);

    window_->clear_chart();
    QTRY_COMPARE_WITH_TIMEOUT(view->point_count(kH1), 0, kWaitMs);  // redrawn by the window's timer
    QCOMPARE(view->point_count(kAX), 0);
    QCOMPARE(window_->chart_model().latest_x(), 0.0);
  }

  void integrationChangeShowsSnappedActual() {
    open();
    QVERIFY(window_->actual_integration_text().isEmpty());
    window_->show();
    QTRY_COMPARE_WITH_TIMEOUT(window_->actual_integration_text(), QStringLiteral("0.1 s"), kWaitMs);

    window_->choose_integration(0.5);  // a preset
    QTRY_COMPARE_WITH_TIMEOUT(window_->actual_integration_text(), QStringLiteral("0.5 s"), kWaitMs);

    // Not a multiple of the sim's 0.1 s quantum: the label shows what it chose.
    window_->choose_integration(0.25);
    QTRY_VERIFY_WITH_TIMEOUT(window_->actual_integration_text() == QStringLiteral("0.2 s") ||
                                 window_->actual_integration_text() == QStringLiteral("0.3 s"),
                             kWaitMs);
    QVERIFY(sim_->scan->running());
    QVERIFY(window_->banner_text().isEmpty());
  }

  void applyPositionsAndUpdatesLabels() {
    open();
    QVERIFY(window_->position_text().isEmpty());  // nothing read or moved yet
    QVERIFY(window_->apply_enabled());

    window_->select_target("H1", "Ar40");
    apply_and_wait();
    QCOMPARE(moves_, 1);
    QVERIFY(!window_->position_text().isEmpty());
    QVERIFY(window_->mass_text().startsWith(QStringLiteral("39.9")));
    QCOMPARE(isotope_shown(kH1), QStringLiteral("Ar40"));

    window_->select_target("H1", "Ar36");
    apply_and_wait();
    QCOMPARE(moves_, 2);
    QVERIFY(window_->mass_text().startsWith(QStringLiteral("35.9")));
    QCOMPARE(isotope_shown(kH1), QStringLiteral("Ar36"));
    QCOMPARE(isotope_shown(kAX), QStringLiteral("Ar39"));  // only the positioned detector changes
    QVERIFY(window_->banner_text().isEmpty());
  }

  void changingCombosAloneMovesNothing() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->running(), kWaitMs);
    window_->select_target("H1", "Ar36");
    window_->select_target("AX", "Ar40");
    window_->select_target("CDD", "Ar36");
    window_->select_target("ZZ", "Ar40");  // unknown detector: ignored
    window_->select_target("H1", "Xx99");  // unknown isotope: ignored
    bridge_->drain();
    QTest::qWait(200);
    QCOMPARE(moves_, 0);
    QCOMPARE(positions_, 0);
    QVERIFY(window_->apply_enabled());
  }

  void largeMoveAsksAndDecliningSendsNothing() {
    seed("confirm_move_amu", 2.0);
    open();
    // The current mass is unknown (no scan, no move yet): that counts as large.
    answer_moves(true);
    window_->select_target("H1", "Ar40");
    apply_and_wait();
    QCOMPARE(asked_.size(), std::size_t{1});
    QVERIFY(std::isnan(asked_.front()));
    QCOMPARE(moves_, 1);

    answer_moves(false);
    window_->select_target("H1", "Ar36");
    window_->apply_position();
    QCOMPARE(asked_.size(), std::size_t{1});
    QVERIFY2(std::abs(std::abs(asked_.front()) - 4.0) < 0.1, qPrintable(QString::number(asked_.front())));
    QVERIFY(window_->apply_enabled());  // declined: nothing is pending
    bridge_->drain();
    QTest::qWait(200);
    QCOMPARE(moves_, 1);
    QCOMPARE(positions_, 1);
    QCOMPARE(isotope_shown(kH1), QStringLiteral("Ar40"));
  }

  void smallMoveDoesNotAsk() {
    // Tighter than one mass unit, so that comparing isotope masses instead of
    // the mass on the reference detector would ask below.
    seed("confirm_move_amu", 0.5);
    open();
    window_->select_target("H1", "Ar39");
    apply_and_wait();

    // Ar40 on AX leaves H1 (the reference) on Ar39: a small move, although the
    // isotopes are one mass unit apart.
    answer_moves(false);
    const auto on_reference = bridge_->mass_on_reference_for("Ar40", "AX");
    QVERIFY(on_reference.has_value());
    QVERIFY(std::abs(*on_reference - *bridge_->state().mass_on_reference) < 0.1);
    window_->select_target("AX", "Ar40");
    apply_and_wait();
    QVERIFY(asked_.empty());
    QCOMPARE(moves_, 2);
    QCOMPARE(isotope_shown(kAX), QStringLiteral("Ar40"));

    // Back to Ar40 on the reference itself is one mass unit: asked.
    window_->select_target("H1", "Ar40");
    window_->apply_position();
    QCOMPARE(asked_.size(), std::size_t{1});
    QVERIFY(std::abs(asked_.front() - 1.0) < 0.1);
  }

  void thresholdZeroNeverAsks() {
    seed("confirm_move_amu", 0.0);
    open();
    answer_moves(false);
    window_->select_target("H1", "Ar36");  // current mass unknown, and far from wherever the magnet is
    apply_and_wait();
    window_->select_target("H1", "Ar40");
    apply_and_wait();
    QVERIFY(asked_.empty());
    QCOMPARE(moves_, 2);
  }

  void stallShowsBannerAndRestartClearsIt() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(0) >= 1, kWaitMs);
    QVERIFY(window_->banner_text().isEmpty());

    sim_->bus.publish(Alarm{"acquisition", AlarmSeverity::Critical, "no frames for 2 s", sim_->clock.now()});
    QTRY_VERIFY_WITH_TIMEOUT(window_->banner_text().contains(QStringLiteral("no frames")), kWaitMs);

    window_->restart_scan();
    QTRY_VERIFY_WITH_TIMEOUT(window_->banner_text().isEmpty(), kWaitMs);
    QVERIFY(sim_->scan->running());
    const int before = window_->chart_view()->point_count(0);
    QTRY_VERIFY_WITH_TIMEOUT(window_->chart_view()->point_count(0) > before, kWaitMs);
  }

  void failedCommandShowsBanner() {
    open();
    window_->select_target("H1", "Ar36");
    bridge_->position("Xx99", "H1");  // the combos cannot name an unknown isotope
    QTRY_VERIFY_WITH_TIMEOUT(!window_->banner_text().isEmpty(), kWaitMs);
    QVERIFY(window_->banner_text().startsWith(QStringLiteral("position")));
    QCOMPARE(moves_, 0);
    QVERIFY(window_->apply_enabled());
    QCOMPARE(isotope_shown(kH1), QStringLiteral("Ar40"));

    window_->choose_integration(0.2);  // the next success clears it
    QTRY_VERIFY_WITH_TIMEOUT(window_->banner_text().isEmpty(), kWaitMs);
  }

  // ---- the panel layout -------------------------------------------------------

  void resetPutsTheDocksBack() {
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    auto* controls = window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerControlsDock"));
    auto* intensities = window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerIntensitiesDock"));
    QVERIFY(controls != nullptr && intensities != nullptr);
    intensities->close();
    controls->setFloating(true);

    QVERIFY(window_->dock_layouts() != nullptr);
    window_->dock_layouts()->reset();
    for (const QDockWidget* dock : {controls, intensities}) {
      QVERIFY2(dock->isVisible(), qPrintable(dock->objectName()));
      QVERIFY2(!dock->isFloating(), qPrintable(dock->objectName()));
    }
    QCOMPARE(window_->dockWidgetArea(controls), Qt::LeftDockWidgetArea);
    QCOMPARE(window_->dockWidgetArea(intensities), Qt::RightDockWidgetArea);
  }

  void resetGivesThePanelsTheirSizesBack() {
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    auto* controls = window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerControlsDock"));
    auto* intensities = window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerIntensitiesDock"));
    QTRY_VERIFY(intensities->width() > 0);
    const int controls_width = controls->width();
    const int intensities_width = intensities->width();
    window_->resizeDocks({intensities}, {intensities_width + 200}, Qt::Horizontal);
    QTRY_VERIFY(intensities->width() > intensities_width + 100);

    window_->dock_layouts()->reset();
    constexpr int kSlackPx = 6;
    QTRY_VERIFY2(qAbs(intensities->width() - intensities_width) <= kSlackPx,
                 qPrintable(QStringLiteral("%1 vs %2").arg(intensities->width()).arg(intensities_width)));
    QTRY_VERIFY2(qAbs(controls->width() - controls_width) <= kSlackPx,
                 qPrintable(QStringLiteral("%1 vs %2").arg(controls->width()).arg(controls_width)));
  }

  void panelsLeaveOutTheControls() {
    open();
    QVERIFY(window_->dock_layouts() != nullptr);
    QStringList panels;
    for (const QAction* action : window_->dock_layouts()->panel_actions()) panels.append(action->text());
    QCOMPARE(panels, (QStringList{QStringLiteral("Intensities"), QStringLiteral("Peak center"),
                                  QStringLiteral("Peak center plot")}));
  }

  // ---- the peak center docks ----------------------------------------------------

  void peakCenterDocksPresent() {
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    auto* dock = window_->findChild<QDockWidget*>(QStringLiteral("peakCenterDock"));
    auto* plot = window_->findChild<QDockWidget*>(QStringLiteral("peakCenterPlotDock"));
    QVERIFY(dock != nullptr && plot != nullptr);
    QCOMPARE(dock->widget(), window_->peak_center_panel());
    QCOMPARE(plot->widget(), window_->peak_center_view());
    QVERIFY(window_->tabifiedDockWidgets(window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerControlsDock")))
                .contains(dock));
    QVERIFY(plot->isVisible());
    QVERIFY(!window_->tabifiedDockWidgets(plot).contains(dock));
    const auto actions = window_->dock_layouts()->panel_actions();
    QVERIFY(actions.contains(dock->toggleViewAction()));
    QVERIFY(actions.contains(plot->toggleViewAction()));
    QVERIFY(!window_->peak_center_running());
  }

  void peakCenterDockStateIsSaved() {
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    window_->findChild<QDockWidget*>(QStringLiteral("peakCenterPlotDock"))->close();
    window_->close();
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    QVERIFY(!window_->findChild<QDockWidget*>(QStringLiteral("peakCenterPlotDock"))->isVisible());
  }

  void cancelAndWaitEndsTheRunWithoutAsking() {
    open();
    const auto file = std::filesystem::path(dir_.filePath(QStringLiteral("pc.toml")).toStdString());
    std::ofstream(file) << "[default]\nisotope = \"Ar40\"\nwindow = 0.06\nstep = 0.0005\nintegration_s = 0.1\n"
                           "settle_s = 0.1\n";
    window_->set_peak_center_file(file);
    int asked = 0;
    window_->set_ask([&](const QString&) {
      ++asked;
      return true;
    });
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    QTest::mouseClick(window_->findChild<QPushButton*>(QStringLiteral("peakCenterStart")), Qt::LeftButton);
    QVERIFY(window_->peak_center_running());
    window_->cancel_peak_center_and_wait();
    QVERIFY(!window_->peak_center_running());
    QVERIFY(window_->close());
    QCOMPARE(asked, 0);
  }

  void closeDuringRunAsks() {
    open();
    const auto file = std::filesystem::path(dir_.filePath(QStringLiteral("pc.toml")).toStdString());
    std::ofstream(file) << "[default]\nisotope = \"Ar40\"\nwindow = 0.06\nstep = 0.0005\nintegration_s = 0.1\n"
                           "settle_s = 0.1\n";
    window_->set_peak_center_file(file);
    QStringList asked;
    bool answer = false;
    window_->set_ask([&](const QString& q) {
      asked << q;
      return answer;
    });
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    QTest::mouseClick(window_->findChild<QPushButton*>(QStringLiteral("peakCenterStart")), Qt::LeftButton);
    QVERIFY(window_->peak_center_running());

    QVERIFY(!window_->close());  // no: the window stays, the run goes on
    QCOMPARE(asked, QStringList{QStringLiteral("Cancel peak center?")});
    QVERIFY(window_->isVisible());
    QVERIFY(window_->peak_center_running());

    QStringList commands;
    connect(bridge_.get(), &SpectrometerBridge::commandFinished, this,
            [&commands](const QString& what, const Result<void>&) { commands << what; });
    answer = true;
    window_->close();  // yes: cancel, close when it has finished
    QCOMPARE(asked.size(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!window_->isVisible(), kWaitMs);
    QCOMPARE(finished.count(), 1);
    bridge_->drain();
    QCoreApplication::processEvents();
    QVERIFY2(!commands.contains(QStringLiteral("start")), qPrintable(commands.join(u',')));  // no scan put back to be stopped
    QVERIFY(!sim_->scan->status().running);
    QVERIFY(!window_->peak_center_running());
    QVERIFY(!finished.at(0).at(0).value<Result<jobs::PeakCenterResult>>().has_value());  // Cancelled
  }

  // While the window's peak center runs, nothing in the Controls dock may move
  // the magnet or start a scan under the sweep: Apply, the banner's Restart
  // and the rise rate's Start are disabled from Start until the run has
  // finished, and a rise rate run going is ended. Stop scan is not touched.
  void controlsAreLockedWhileAPeakCenterRuns() {
    open();
    write_pc_file();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    auto* restart = window_->findChild<QPushButton*>(QStringLiteral("scanRestart"));
    QVERIFY(restart != nullptr);
    auto* rise = window_->rise_rate()->toggle_button();
    QVERIFY(window_->apply_enabled());
    QVERIFY(restart->isEnabled());
    QVERIFY(rise->isEnabled());
    window_->rise_rate()->toggle();
    QVERIFY(window_->rise_rate()->running());

    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    QTest::mouseClick(window_->findChild<QPushButton*>(QStringLiteral("peakCenterStart")), Qt::LeftButton);
    QVERIFY(window_->peak_center_running());
    QVERIFY(!window_->apply_enabled());
    QVERIFY(!restart->isEnabled());
    QVERIFY(!rise->isEnabled());
    QVERIFY(!window_->rise_rate()->running());
    // The programmatic doors are shut too.
    const int before = positions_;
    window_->apply_position();
    window_->restart_scan();
    QVERIFY(window_->peak_center_running());

    bridge_->cancel_peak_center();
    QVERIFY(finished.wait(kWaitMs));
    QVERIFY(!window_->peak_center_running());
    QVERIFY(window_->apply_enabled());
    QVERIFY(restart->isEnabled());
    QVERIFY(rise->isEnabled());
    bridge_->drain();
    QCoreApplication::processEvents();
    QCOMPARE(positions_, before);
  }

  void closeWithoutRunDoesNotAsk() {
    open();
    bool asked = false;
    window_->set_ask([&](const QString&) {
      asked = true;
      return false;
    });
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    window_->close();
    QVERIFY(!asked);
    QVERIFY(!window_->isVisible());
  }

  void anArrangementIsKeptUnderTheSpectrometersName() {
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    QVERIFY(window_->dock_layouts() != nullptr);
    QVERIFY(window_->dock_layouts()->save_as(QStringLiteral("scan")));
    QVERIFY(settings()->contains(QStringLiteral("spectrometer_window/sim-integrated/arrangements/scan/state")));
  }

  void theDockLayoutComesBackUnderItsOldKey() {
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerIntensitiesDock"))->close();
    QVERIFY(window_->close());
    QVERIFY(!saved("dock_state").toByteArray().isEmpty());
    QVERIFY(!saved("geometry").toByteArray().isEmpty());
    window_.reset();
    open();
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    QVERIFY(!window_->findChild<QDockWidget*>(QStringLiteral("SpectrometerIntensitiesDock"))->isVisible());
  }

  void closingStopsScanAndSavesSettings() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->running(), kWaitMs);
    window_->set_scan_width_minutes(3.0);

    QVERIFY(window_->close());
    QTRY_VERIFY_WITH_TIMEOUT(!sim_->scan->running(), kWaitMs);
    QCOMPARE(saved("scan_width_s").toDouble(), 180.0);
    QCOMPARE(saved("integration_s").toDouble(), 0.1);
    QCOMPARE(saved("confirm_move_amu").toDouble(), 5.0);  // the default is written out

    // The window is created once and shown again: the scan comes back.
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->running(), kWaitMs);
  }

  void settingsRoundTrip() {
    open();
    window_->set_scan_width_minutes(2.0);
    window_->set_scale(YScale::Log);
    window_->set_autoscale(false);
    QVERIFY(window_->set_manual_y(0.5, 5000.0));
    window_->set_detector_shown("L2", false);
    window_->select_target("AX", "Ar36");
    window_->choose_integration(0.5);
    QVERIFY(window_->close());
    window_.reset();

    open();
    auto& model = window_->chart_model();
    QCOMPARE(model.scan_width(), 120.0);
    QCOMPARE(model.scale(), YScale::Log);
    QVERIFY(!model.autoscale());
    const AxisRange y = model.y_range(std::chrono::steady_clock::now());
    QCOMPARE(y.lo, 0.5);
    QCOMPARE(y.hi, 5000.0);
    for (const auto& detector : model.detectors()) {
      QCOMPARE(detector.visible, detector.name != "L2");
    }
    window_->chart_view()->refresh();
    QVERIFY(!window_->chart_view()->graph_visible(4));  // L2
    QCOMPARE(window_->chart_view()->shown_y().hi, 5000.0);

    // The target and the integration are restored into the controls: Apply and
    // show use them.
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->running(), kWaitMs);
    QCOMPARE(sim_->scan->integration(), Duration(500ms));
    apply_and_wait();
    QCOMPARE(isotope_shown(kAX), QStringLiteral("Ar36"));
  }

  void staleOrCorruptSettingsIgnored() {
    seed("detector", "ZZ");
    seed("isotope", "Xx99");
    seed("hidden_detectors", QStringList{"ZZ", "QQ"});
    seed("scan_width_s", "abc");
    seed("scale", "sideways");
    seed("autoscale", "maybe");
    seed("ymin", 10.0);
    seed("ymax", 1.0);
    seed("integration_s", "fast");
    seed("confirm_move_amu", "-3");
    seed("geometry", QByteArray("not a geometry"));
    seed("dock_state", QByteArray("not a dock state"));
    open();

    auto& model = window_->chart_model();
    QCOMPARE(model.scan_width(), 60.0);
    QCOMPARE(model.scale(), YScale::Linear);
    QVERIFY(model.autoscale());
    for (const auto& detector : model.detectors()) {
      QVERIFY(detector.visible);
    }
    window_->chart_view()->refresh();
    QVERIFY(window_->chart_view()->shown_y().lo < window_->chart_view()->shown_y().hi);

    // Defaults: the config's integration, the reference detector with its own
    // isotope, and a 5 amu threshold (Ar40 -> Ar36 is 4: not asked).
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->running(), kWaitMs);
    QCOMPARE(sim_->scan->integration(), Duration(1s));
    answer_moves(true);
    apply_and_wait();
    QCOMPARE(isotope_shown(kH1), QStringLiteral("Ar40"));
    answer_moves(false);
    window_->select_target("H1", "Ar36");
    apply_and_wait();
    QVERIFY(asked_.empty());
    QVERIFY(window_->close());
    QCOMPARE(saved("confirm_move_amu").toDouble(), 5.0);
  }

  void closeWithCommandInFlightIsClean() {
    open();
    window_->show();
    QTRY_VERIFY_WITH_TIMEOUT(sim_->scan->running(), kWaitMs);
    window_->select_target("H1", "Ar36");
    window_->apply_position();
    window_->choose_integration(0.2);
    QVERIFY(window_->close());  // the move is still running on the executor
    window_.reset();
    QCOMPARE(saved("isotope").toString(), QStringLiteral("Ar36"));

    // The commands finish in order with nobody listening; the stop is last.
    bridge_->drain();
    QCoreApplication::processEvents();
    QVERIFY(!sim_->scan->running());
    QCOMPARE(moves_, 1);
  }
  // Every window here was given its settings: none fell back to the application's.
  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(TestSpectrometerWindow)
#include "test_spectrometer_window.moc"
