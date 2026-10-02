// Build smoke for the plotting stack: QCustomPlot links and renders, and the
// sim spectrometer drivers (registered from static initializers) are present
// in this executable.

#include <chrono>
#include <filesystem>

#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

class TestPlotBuild : public QObject {
  Q_OBJECT

 private slots:
  void qcustomplotRenders() {
    QCustomPlot plot;
    plot.resize(200, 100);
    QCPGraph* graph = plot.addGraph();
    graph->setData({0.0, 1.0, 2.0}, {1.0, 3.0, 2.0});
    plot.rescaleAxes();
    plot.replot();
    QVERIFY(!plot.toPixmap(200, 100).isNull());
  }

  void simSpectrometerDriversRegistered() {
    auto data =
        cfg::load_spectrometer(std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "spectrometer.sim-integrated.toml");
    if (!data) QFAIL(data.error().what.c_str());
    ManualClock clock{TimePoint{} + 1000s};
    SignalBus bus;
    Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
    auto spec = SpectrometerAssembler::assemble(std::move(*data), SpectrometerContext{clock, scheduler, bus});
    if (!spec) QFAIL(spec.error().what.c_str());
  }
};

QTEST_MAIN(TestPlotBuild)
#include "test_plot_build.moc"
