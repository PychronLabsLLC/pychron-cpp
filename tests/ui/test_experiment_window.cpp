// ExperimentWindow end to end on the sim lab (simulated time paced at 400x): open the example
// queue, run it, follow it; confirmations, saving, and View > Experiment.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QMenu>
#include <QMenuBar>
#include <QTabBar>
#include <QLabel>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QToolButton>
#include <QTemporaryDir>
#include <QDockWidget>
#include <QToolBar>
#include <QtTest/QtTest>

#include "conditionals_editor_window.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"
#include "reference_series_dock.hpp"
#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "peak_center_view.hpp"
#include "shortcuts.hpp"
#include "menu_hub.hpp"
#include "preferences_dialog.hpp"
#include "script_editor_window.hpp"
#include "settings_guard.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/queue_toml.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/trace_recorder.hpp"

using pychron::experiment::run::RunState;
using pychron::ui::ExperimentBridge;
using pychron::ui::ExperimentWindow;
using pychron::ui::QueueTableModel;
namespace fs = std::filesystem;


// An identifier source that answers at once, with one package.
struct OnePackageSource : pychron::ui::IdentifierSource {
  void packages(QObject*, std::function<void(pychron::Result<std::vector<pychron::ui::PackageChoice>>)> done) override {
    done(std::vector<pychron::ui::PackageChoice>{{"p1", "NM-293"}});
  }
  void contents(QObject*, const std::string&,
                std::function<void(pychron::Result<pychron::ui::PackageContents>)> done) override {
    done(pychron::ui::PackageContents{});
  }
};

// A database that will not take the queue: check_queue says why, in two lines.
struct RefusingPersister : pychron::experiment::persist::IAnalysisPersister {
  pychron::Result<int> next_aliquot(const std::string&) override { return 1; }
  pychron::Result<void> begin_run(const pychron::experiment::RunIdentity&,
                                  const pychron::experiment::QueueSpec&) override {
    return {};
  }
  pychron::Result<void> save_extraction(const pychron::experiment::record::AnalysisRecord&) override { return {}; }
  pychron::Result<void> save_analysis(const pychron::experiment::record::AnalysisRecord&) override { return {}; }
  pychron::Result<void> save_artifact(const std::string&, const std::string&,
                                      const std::vector<std::uint8_t>&) override {
    return {};
  }
  pychron::Result<void> flush() override { return {}; }
  pychron::Result<std::vector<std::string>> check_queue(const pychron::experiment::QueueSpec&) override {
    return std::vector<std::string>{"the database cannot be reached: no such file",
                                    "Run: elctl db migrate --db sqlite:/lab/data/pychron.db"};
  }
};

// What the References dock reads: counts the refreshes the dock asks the
// bridge for (one a tab a read), and can hold one until released.
// A series key at the plan's first place (gas "", segment 0, position 0).
static pychron::experiment::collect::SeriesKey skey(std::string label, std::string detector, std::string_view kind) {
  pychron::experiment::collect::SeriesKey k;
  k.label = std::move(label);
  k.detector = std::move(detector);
  k.kind = std::string(kind);
  return k;
}

class RefreshCountingSource : public pychron::processing::MemorySource {
 public:
  pychron::Result<void> refresh() override {
    std::unique_lock lock(mutex_);
    ++refreshes_;
    if (closed_) {
      ++held_;
      gate_.wait(lock, [this] { return !closed_; });
      --held_;
    }
    return {};
  }
  int refreshes() const {
    const std::lock_guard lock(mutex_);
    return refreshes_;
  }
  int held() const {
    const std::lock_guard lock(mutex_);
    return held_;
  }
  void close() {
    const std::lock_guard lock(mutex_);
    closed_ = true;
  }
  void open() {
    {
      const std::lock_guard lock(mutex_);
      closed_ = false;
    }
    gate_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable gate_;
  bool closed_ = false;
  int refreshes_ = 0;
  int held_ = 0;
};

// What set_references is given, declared before any window that is given it.
struct References {
  QTemporaryDir dir;
  RefreshCountingSource source;
  std::unique_ptr<pychron::ui::ProcessingBridge> bridge = std::make_unique<pychron::ui::ProcessingBridge>(source);
  pychron::processing::PresetStore presets{(dir.path() + QStringLiteral("/presets")).toStdString()};
};

pychron::experiment::executor::RunFinished finished_run(bool save_error) {
  pychron::experiment::executor::RunFinished e;
  e.summary.save_error = save_error;
  return e;
}

class TestExperimentWindow : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;

  std::unique_ptr<QSettings> settings() const {
    return std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
  }
  // The example queue as the fixture runs it (scripts dropped without Python).
  fs::path queue_file(const pychron::ui::test::SimLab& sim, const char* name = "queue.toml") const {
    const fs::path p = sim.dir / name;
    if (!pychron::experiment::save_queue_file(p.string(), sim.queue())) qFatal("cannot write the queue");
    return p;
  }
  // Settings of their own, for a test that needs a list nothing else wrote to.
  static std::unique_ptr<QSettings> settings_in(const QTemporaryDir& dir) {
    return std::make_unique<QSettings>(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
  }
  static QString canonical(const fs::path& p) { return QString::fromStdString(fs::weakly_canonical(p).string()); }
  // Shows `window` and brings it in front: its Window-scope actions are
  // enabled only then. False where the platform does not activate windows.
  static bool shown_active(QWidget& window) {
    window.show();
    window.activateWindow();
    return QTest::qWaitForWindowActive(&window);
  }
  // The File > Open Recent action with `text`, after the menu is rebuilt.
  static QAction* recent_entry(const ExperimentWindow& window, const QString& text) {
    emit window.recent_menu()->aboutToShow();
    for (QAction* a : window.recent_menu()->actions())
      if (a->text() == text) return a;
    return nullptr;
  }
  // The title of the dock in front among those tabbed with `title`, or empty.
  static QString in_front_of(const QMainWindow& window, const QString& title) {
    for (const QTabBar* bar : window.findChildren<QTabBar*>(QString(), Qt::FindDirectChildrenOnly))
      for (int i = 0; i < bar->count(); ++i)
        if (bar->tabText(i) == title) return bar->tabText(bar->currentIndex());
    return {};
  }
  // The executor's state file as a crash or a Stop left it, in the session's data dir.
  static fs::path write_state(const pychron::ui::test::SimLab& sim, const std::string& text) {
    const fs::path p = sim.dir / "data" / "executor_state.json";
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
    return p;
  }
  // A state file for the example queue: stopped at `next_row`, row 0 finished `row0_state`
  // under `row0_identifier` (the queue's own when empty).
  static fs::path write_example_state(const pychron::ui::test::SimLab& sim, std::size_t next_row,
                                      std::string row0_identifier = {}, const std::string& state = "running") {
    const auto q = sim.queue();
    if (row0_identifier.empty()) row0_identifier = q.runs[0].id.identifier;
    return write_state(sim, R"({"queue": ")" + q.name + R"(", "state": ")" + state + R"(", "next_row": )" +
                                std::to_string(next_row) + R"(, "runs": [{"row": 0, "identifier": ")" +
                                row0_identifier + R"(", "run_id": "r1", "state": "success"}]})");
  }
  static bool contains(const QStringList& lines, const QString& text) {
    for (const auto& l : lines)
      if (l.contains(text)) return true;
    return false;
  }
  static int count(const QStringList& lines, const QString& text) {
    return static_cast<int>(std::count_if(lines.begin(), lines.end(), [&](const QString& l) { return l.contains(text); }));
  }

  // A held queue (spec 4.6): the session made again with a settle of an hour,
  // so a link that is back does not end the hold of itself while the test
  // looks, and the spectrometer's first link closed and kept down, so the
  // first run is held for. Returns that link's simulated transport.
  static pychron::SimTransport* hold_the_first_run(pychron::ui::test::SimLab& sim) {
    namespace lab = pychron::experiment::lab;
    sim.session.reset();
    sim.lab.executor.hold.settle_s = 3600;
    sim.lab.executor.hold.limit_s = 30.0 * 24 * 3600;  // at 400x the default 2 h is 18 s: a slow runner could reach it
    sim.session = std::make_unique<lab::LabSession>(
        sim.lab, lab::SessionHardware{*sim.line, sim.spec.get(), sim.scan.get(), {}},
        lab::SessionOptions{sim.dir / "data", {}, {}, nullptr});
    const auto links = sim.spec->links();
    if (links.empty()) return nullptr;
    pychron::Transport* t = &links.front()->transport();
    if (auto* recorder = dynamic_cast<pychron::TraceRecorder*>(t)) t = &recorder->inner();
    auto* down = dynamic_cast<pychron::SimTransport*>(t);
    if (down == nullptr) return nullptr;
    down->set_down(true);
    links.front()->transport().close();
    return down;
  }

 private slots:
  // ---- the panel layout -------------------------------------------------------

  void resetPutsTheDocksAndTheToolbarBack() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* executor = window.findChild<QDockWidget*>(QStringLiteral("ExperimentExecutorDock"));
    auto* evolutions = window.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"));
    auto* factory = window.findChild<QDockWidget*>(QStringLiteral("ExperimentFactoryDock"));
    auto* measurement = window.findChild<QDockWidget*>(QStringLiteral("ExperimentMeasurementDock"));
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("ExperimentToolBar"));
    QVERIFY(executor != nullptr && evolutions != nullptr && factory != nullptr && measurement != nullptr);
    QVERIFY(toolbar != nullptr);
    evolutions->close();
    measurement->setFloating(true);
    toolbar->hide();
    window.addToolBar(Qt::BottomToolBarArea, toolbar);
    window.addDockWidget(Qt::TopDockWidgetArea, executor);

    QVERIFY(window.dock_layouts() != nullptr);
    window.dock_layouts()->reset();
    for (const QDockWidget* dock : {executor, evolutions, factory, measurement}) {
      QVERIFY2(!dock->isHidden(), qPrintable(dock->objectName()));
      QVERIFY2(!dock->isFloating(), qPrintable(dock->objectName()));
    }
    QCOMPARE(window.dockWidgetArea(executor), Qt::BottomDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(evolutions), Qt::RightDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(factory), Qt::LeftDockWidgetArea);
    QVERIFY(window.tabifiedDockWidgets(factory).contains(measurement));
    QVERIFY(factory->isVisible());  // in front of Measurement
    QVERIFY(toolbar->isVisible());
    QCOMPARE(window.toolBarArea(toolbar), Qt::TopToolBarArea);
  }

  // A staged release's prepare and pump-out (spec 11.4) are named with their gas.
  void aPrepareAndAPumpOutAreNamedWithTheirGas() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    auto* pane = window.executor();
    namespace meas = pychron::experiment::measurement;
    sim.line->bus().publish(meas::BlockStarted{"r1", meas::Block::Prepare, "He", -1});
    QTRY_VERIFY2(pane->run_text().contains(QStringLiteral("prepare He")), qPrintable(pane->run_text()));
    sim.line->bus().publish(meas::BlockStarted{"r1", meas::Block::PumpOut, "He", -1});
    QTRY_VERIFY2(pane->run_text().contains(QStringLiteral("pump out He")), qPrintable(pane->run_text()));
    sim.line->bus().publish(meas::BlockStarted{"r1", meas::Block::Main, "Ne", 0});
    QTRY_VERIFY2(pane->run_text().endsWith(QStringLiteral("main")), qPrintable(pane->run_text()));
  }

  void panelsLeaveOutTheExecutor() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    const ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.dock_layouts() != nullptr);
    QStringList panels;
    for (const QAction* action : window.dock_layouts()->panel_actions()) panels.append(action->text());
    QCOMPARE(panels, QStringList({QStringLiteral("Evolutions"), QStringLiteral("Measurement"), QStringLiteral("Run Factory")}));
  }

  void theLayoutIsStillKeptUnderItsOldKeys() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    {
      ExperimentWindow window(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      window.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"))->close();
      QVERIFY(window.close());
    }
    {
      const QSettings saved(file, QSettings::IniFormat);
      QVERIFY(saved.contains(QStringLiteral("experiment_window/state")));
      QVERIFY(saved.contains(QStringLiteral("experiment_window/geometry")));
    }
    ExperimentWindow again(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    QVERIFY(!again.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"))->isVisible());
    QVERIFY(again.dock_layouts() != nullptr);
    QVERIFY(again.dock_layouts()->save_as(QStringLiteral("quiet")));
    again.dock_layouts()->reset();
    QVERIFY(again.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"))->isVisible());
    QVERIFY(again.dock_layouts()->apply(QStringLiteral("quiet")));
    QVERIFY(!again.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"))->isVisible());
  }

  // ---- the References dock -------------------------------------------------------

  void aWindowGivenReferencesHasTheDock() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    ExperimentWindow window(bridge, true, settings());
    window.set_references(refs.bridge.get(), &refs.presets);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock"));
    auto* evolutions = window.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"));
    QVERIFY(dock != nullptr && evolutions != nullptr);
    QVERIFY(window.references() != nullptr);
    QCOMPARE(dock->widget(), window.references());
    QCOMPARE(dock->windowTitle(), QStringLiteral("References"));
    QVERIFY(window.tabifiedDockWidgets(evolutions).contains(dock));
    QCOMPARE(window.dockWidgetArea(dock), Qt::RightDockWidgetArea);
    QVERIFY(!dock->isFloating());
    QTRY_COMPARE(in_front_of(window, QStringLiteral("References")), QStringLiteral("Evolutions"));
    QStringList panels;
    for (const QAction* action : window.dock_layouts()->panel_actions()) panels.append(action->text());
    QVERIFY2(panels.contains(QStringLiteral("References")), qPrintable(panels.join(QLatin1Char(','))));

    // Reset Layout puts it back behind the evolutions.
    dock->setFloating(true);
    window.dock_layouts()->reset();
    QVERIFY(!dock->isFloating());
    QVERIFY(window.tabifiedDockWidgets(evolutions).contains(dock));
    QTRY_COMPARE(in_front_of(window, QStringLiteral("References")), QStringLiteral("Evolutions"));
    window.set_references(nullptr, nullptr);
  }

  void aWindowNotGivenThemHasNone() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock")) == nullptr);
    QVERIFY(window.references() == nullptr);
    window.set_references(refs.bridge.get(), nullptr);  // both, or none
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock")) == nullptr);
    window.set_references(nullptr, &refs.presets);
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock")) == nullptr);
    for (const QAction* action : window.dock_layouts()->panel_actions())
      QVERIFY(action->text() != QStringLiteral("References"));
  }

  void aSavedRunIsFollowedByARead() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    ExperimentWindow window(bridge, true, settings());
    window.set_references(refs.bridge.get(), &refs.presets);
    QCOMPARE(refs.source.refreshes(), 0);  // behind the evolutions: not read yet
    emit bridge.runFinished(finished_run(false));
    QTRY_COMPARE(refs.source.refreshes(), 2);  // a tab, a refresh
    QVERIFY(refs.bridge->wait_idle());
    window.set_references(nullptr, nullptr);
  }

  void aRunThatWasNotSavedIsNot() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    ExperimentWindow window(bridge, true, settings());
    window.set_references(refs.bridge.get(), &refs.presets);
    emit bridge.runFinished(finished_run(true));
    QCoreApplication::processEvents();  // the connection is queued
    QVERIFY(refs.bridge->wait_idle());
    QCoreApplication::processEvents();
    QCOMPARE(refs.source.refreshes(), 0);
    window.set_references(nullptr, nullptr);
  }

  // The dock draws the queue's spectrometer: a queue of another is read for.
  void aQueueOfAnotherSpectrometerIsReadFor() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    ExperimentWindow window(bridge, true, settings());
    window.set_references(refs.bridge.get(), &refs.presets);
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim), &error), qPrintable(error));  // "sim", from none
    QTRY_COMPARE(refs.source.refreshes(), 2);
    QVERIFY(refs.bridge->wait_idle());
    QVERIFY2(window.load_queue(queue_file(sim, "same.toml"), &error), qPrintable(error));
    QVERIFY(refs.bridge->wait_idle());
    QCoreApplication::processEvents();
    QCOMPARE(refs.source.refreshes(), 2);  // the same spectrometer: nothing to read again
    QVERIFY(window.new_queue(&error));     // none: any
    QTRY_COMPARE(refs.source.refreshes(), 4);
    QVERIFY(refs.bridge->wait_idle());
    window.set_references(nullptr, nullptr);
  }

  void aLayoutSavedWithTheDockPlacesItAgain() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    {
      ExperimentWindow window(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
      window.set_references(refs.bridge.get(), &refs.presets);
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      window.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock"))->close();
      QVERIFY(window.close());  // keeps the layout
      window.set_references(nullptr, nullptr);
    }
    ExperimentWindow again(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.set_references(refs.bridge.get(), &refs.presets);
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    auto* dock = again.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock"));
    QVERIFY(dock != nullptr);
    QVERIFY(dock->isHidden());
    QVERIFY(again.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"))->isVisible());
    again.set_references(nullptr, nullptr);
  }

  void aLayoutSavedBeforeTheDockStillPlacesIt() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    {
      ExperimentWindow window(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      QVERIFY(window.close());  // a layout with no References dock in it
    }
    ExperimentWindow again(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.set_references(refs.bridge.get(), &refs.presets);
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    auto* dock = again.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock"));
    auto* evolutions = again.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"));
    QVERIFY(dock != nullptr && evolutions != nullptr);
    QVERIFY(!dock->isHidden());
    QVERIFY(!dock->isFloating());
    QCOMPARE(again.dockWidgetArea(dock), Qt::RightDockWidgetArea);
    QVERIFY(again.tabifiedDockWidgets(evolutions).contains(dock));
    QTRY_COMPARE(in_front_of(again, QStringLiteral("References")), QStringLiteral("Evolutions"));
    again.set_references(nullptr, nullptr);
  }

  // A named arrangement kept before the window had the dock leaves it shown.
  void anArrangementSavedWithoutTheDockLeavesItShown() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    {
      ExperimentWindow window(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      QVERIFY(window.dock_layouts()->save_as(QStringLiteral("before")));
    }
    ExperimentWindow again(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.set_references(refs.bridge.get(), &refs.presets);
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    QVERIFY(again.dock_layouts()->apply(QStringLiteral("before")));
    auto* dock = again.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock"));
    QVERIFY(dock != nullptr);
    // Where it was when the arrangement was applied: Qt moves only the docks it names.
    QVERIFY(!dock->isHidden());
    QVERIFY(!dock->isFloating());
    QCOMPARE(again.dockWidgetArea(dock), Qt::RightDockWidgetArea);
    again.set_references(nullptr, nullptr);
  }

  // A layout kept with the evolutions closed, and no dock.
  void aLayoutWithTheEvolutionsClosedStillShowsTheDock() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    {
      ExperimentWindow window(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      window.findChild<QDockWidget*>(QStringLiteral("ExperimentEvolutionsDock"))->close();
      QVERIFY(window.close());
    }
    ExperimentWindow again(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.set_references(refs.bridge.get(), &refs.presets);
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    auto* dock = again.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock"));
    QVERIFY(dock != nullptr);
    QVERIFY(!dock->isHidden());
    QVERIFY(!dock->isFloating());
    QCOMPARE(again.dockWidgetArea(dock), Qt::RightDockWidgetArea);
    again.set_references(nullptr, nullptr);
  }

  // As MainWindow::set_data does before the data workspace's bridge goes.
  void closingTheDataFirstTakesTheDockAway() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    References refs;
    ExperimentWindow window(bridge, true, settings());
    window.set_references(refs.bridge.get(), &refs.presets);
    refs.source.close();
    emit bridge.runFinished(finished_run(false));
    QTRY_COMPARE_WITH_TIMEOUT(refs.source.held(), 1, 5000);  // a read in flight
    window.set_references(nullptr, nullptr);
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("ExperimentReferencesDock")) == nullptr);
    QVERIFY(window.references() == nullptr);
    refs.source.open();
    QVERIFY(refs.bridge->wait_idle());
    const int seen = refs.source.refreshes();
    refs.bridge.reset();  // the bridge goes, as set_source destroys it
    emit bridge.runFinished(finished_run(false));
    QCoreApplication::processEvents();  // the connection is queued
    QCOMPARE(refs.source.refreshes(), seen);
  }

  void runsTheExampleQueue() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim), &error), qPrintable(error));
    window.show();
    QVERIFY(window.windowTitle().contains(QStringLiteral("(Simulation)")));
    QVERIFY(window.windowTitle().contains(QStringLiteral("queue.toml")));
    QVERIFY(!window.modified());
    auto* pane = window.executor();
    QVERIFY(pane->start_enabled());
    QCOMPARE(pane->progress_text(), QStringLiteral("0/3 run(s)"));

    // Connected after the pane's own handler, so this sees the bar it set.
    int counts_max = 0;
    connect(&bridge, &ExperimentBridge::countsProgress, this,
            [&] { counts_max = std::max(counts_max, pane->counts_maximum()); });
    // The beam measures what the line's source holds, and the example's
    // extraction script lets a pipette of the tank's air into the line for
    // each run that is not a blank (sim_extract.py).
    QVERIFY(sim.line->sim() != nullptr);
    const auto wall_ms = [&sim] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(sim.clock.wall_now().time_since_epoch()).count();
    };
    const qint64 started_ms = wall_ms();
    pane->request_start();
    QVERIFY(pane->running());
    QVERIFY(window.model().live());  // editable after the rows the executor reached
    QVERIFY(!pane->start_enabled());
    QVERIFY(!window.load_queue(queue_file(sim, "other.toml"), &error));
    QCOMPARE(error, QStringLiteral("a queue is running"));
    // A run moves to Executed when it starts (queue-tables design 2).
    QTRY_VERIFY(window.model().status(0).has_value());
    QTRY_VERIFY(window.executed_table()->model()->rowCount() >= 1);
    QVERIFY(window.executed_section()->isVisible());

    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    for (int row = 0; row < 3; ++row) QCOMPARE(window.model().status(row), std::optional<RunState>(RunState::Success));
    QVERIFY(!window.model().live());
    QCOMPARE(pane->progress_text(), QStringLiteral("3/3 run(s)"));
    QVERIFY(contains(pane->events(), QStringLiteral("queue completed")));
    QVERIFY(contains(pane->events(), QStringLiteral("run 1 66001")));
    QVERIFY(contains(pane->events(), QStringLiteral("peak center Ar40 on H1")));
    // Each event has the time it was seen at, on the line's clock: here a
    // simulated one, so the queue's events span its simulated minutes.
    const auto times = pane->event_times();
    const QStringList shown = pane->event_time_texts();
    QCOMPARE(times.size(), pane->events().size());
    QCOMPARE(shown.size(), times.size());
    for (qsizetype i = 0; i < times.size(); ++i) {
      QVERIFY(times[i].isValid());
      QCOMPARE(shown[i], times[i].toString(QStringLiteral("HH:mm:ss")));
      if (i > 0) QVERIFY(times[i - 1] <= times[i]);
    }
    QVERIFY(times.first().toMSecsSinceEpoch() >= started_ms);
    QVERIFY(times.last().toMSecsSinceEpoch() <= wall_ms());
    QVERIFY2(times.first().msecsTo(times.last()) > 60000, qPrintable(shown.join(QLatin1Char(' '))));
    QCOMPARE(pane->state_text(), QStringLiteral("idle"));
    QVERIFY(pane->spool_text().isEmpty());
    QVERIFY(counts_max > 1);
    QCOMPARE(pane->counts_maximum(), 1);  // the last block (the peak center) has no counts
    // The last run's evolutions and peak center.
    auto* evo = window.evolutions();
    QVERIFY(evo->point_count(pychron::experiment::collect::kSignal) > 0);
    QVERIFY(evo->graph_count() > 0);
    // A panel a signal series, Ar40 on H1 among them.
    QCOMPARE(evo->panel_titles().size(), evo->graph_count());
    QVERIFY(evo->panel_titles().contains(QStringLiteral("Ar40 H1")));
    QVERIFY(evo->title_text().startsWith(QStringLiteral("66001")));
    // The live fits: a curve per signal series and its intercept listed.
    QVERIFY(evo->fit_curve_count() > 0);
    QVERIFY(!evo->intercept_rows().isEmpty());
    const auto ar40 = evo->fit_of(skey("Ar40", "H1", pychron::experiment::collect::kSignal));
    QVERIFY(ar40.has_value());
    // The run measured one pipette of the tank's air, and nothing left of
    // the run before it: within 2 % of a shot worked out from the lab's
    // numbers. The shot is the second (this is the third run, the second
    // after the blank to take one): the tank's Ar40 less the first pipette
    // of it, one pipette of that let into prep, and prep's share of it
    // through the inlet into the source (canvas.toml sizes none of the four,
    // so each is its kind's default). The first shot is 0.2 % larger: 2 %
    // does not tell the two apart, and is not meant to (LabSim's
    // SuccessiveShotsDeclineWithTheTank does); it tells a shot from a source
    // that was not pumped out or took no gas.
    const auto& lab = sim.line->sim()->settings();
    QVERIFY(lab.compositions.contains("air_tank"));
    const double tank = lab.default_volume_cc, prep = lab.default_volume_cc, source = lab.default_volume_cc;
    const double pipette = lab.pipette_cc;
    const double filled = lab.compositions.at("air_tank")[pychron::sim::index(pychron::sim::Species::Ar40)] *
                          tank / (tank + pipette);
    const double shot = filled * pipette / (pipette + prep) * prep / (prep + source) * lab.source.sensitivity;
    const double second = shot * tank / (tank + pipette);
    QVERIFY(second > 1e4);
    QVERIFY2(std::abs(ar40->value - second) < 0.02 * second, qPrintable(QString::number(ar40->value)));
    QCOMPARE(evo->peak_centers().size(), 1);
    QVERIFY(evo->peak_centers().front().contains(QStringLiteral("table updated")));
    // The peak center's sweep is drawn on the Peak center tab, the reference first, marked.
    const auto* tabs = evo->findChild<QTabBar*>();
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->tabText(tabs->count() - 1), QStringLiteral("Peak center"));
    const auto drawn = evo->peak_center_view()->series();
    QVERIFY(!drawn.empty());
    QCOMPARE(drawn.front().name, QStringLiteral("*H1"));
    QVERIFY(!drawn.front().x.isEmpty());
    QCOMPARE(evo->peak_center_view()->marker_x().size(), 4);
    QVERIFY(evo->peak_center_view()->kept() >= 1);
    evo->set_kind(pychron::experiment::collect::kBaseline);
    QVERIFY(evo->point_count(pychron::experiment::collect::kBaseline) > 0);
    QVERIFY(evo->graph_count() > 0);
    QVERIFY(fs::exists(sim.dir / "data" / "records" / "66001" / "66001-2.json"));
    // The timeline: a state segment per phase of each run, all closed.
    const auto& tl = pane->timeline();
    QCOMPARE(tl.run_lanes(), 1);  // no overlap in the example queue
    int measuring = 0, waits = 0;
    for (const auto& seg : tl.segments()) {
      QVERIFY(seg.end.has_value());
      measuring += seg.state == pychron::experiment::run::RunState::Measuring ? 1 : 0;
      waits += seg.state ? 0 : 1;
    }
    QCOMPARE(measuring, 3);
    QVERIFY(waits >= 2);  // the delays before the runs
    // Every row was executed: nothing is left to start (queue-tables design 2).
    QCOMPARE(window.model().executed_rows(), 3u);
    QCOMPARE(window.executed_table()->model()->rowCount(), 3);
    QCOMPARE(window.queue_table()->model()->rowCount(), 0);
    QVERIFY(!pane->start_enabled());
    QVERIFY(window.model().reset_executed());
    QVERIFY(pane->start_enabled());
    // A second queue in the same window starts with no peak center kept.
    emit bridge.queueStarted(pychron::experiment::lab::QueueStarted{3, 0});
    QCOMPARE(evo->peak_center_view()->kept(), 0);
    QVERIFY(evo->peak_center_view()->series().empty());
  }

  void notificationsShowInTheExecutorPane() {
    pychron::experiment::lab::NotificationConfig config;
    config.commands.push_back({"log", {"notify-lab"}, {pychron::experiment::lab::NotifyEvent::QueueEnded}});
    std::atomic<int> calls{0};
    pychron::ui::test::SimLab sim(config, [&](const pychron::ProcessSpec&) -> pychron::Result<pychron::ProcessResult> {
      ++calls;
      return pychron::ProcessResult{};
    });
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    auto* pane = window.executor();
    QCOMPARE(pane->notify_text(), QStringLiteral("log"));
    pane->request_start();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    QTRY_VERIFY(contains(pane->events(), QStringLiteral("notified log: pychron: queue sim-example completed")));
    sim.session->notify_test();
    QTRY_VERIFY(contains(pane->events(), QStringLiteral("notified log: pychron: test notification")));
    QCOMPARE(calls.load(), 2);
  }

  // What a run says (here: that its hole was centered) is a line of the
  // pane's events, under the run that said it.
  // Wherever the timeline was left, a queue's start puts it back on now.
  void theTimelineFollowsWhenAQueueStarts() {
    using pychron::ui::TimelineView;
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim), &error), qPrintable(error));
    auto* pane = window.executor();
    QCOMPARE(pane->timeline_view()->mode(), TimelineView::Mode::Follow);
    pane->timeline_view()->set_mode(TimelineView::Mode::WholeQueue);
    pane->request_start();
    QVERIFY(pane->running());
    QCOMPARE(pane->timeline_view()->mode(), TimelineView::Mode::Follow);
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    QCOMPARE(pane->timeline_view()->mode(), TimelineView::Mode::Follow);
    QCOMPARE(pane->timeline().run_starts().size(), std::size_t{3});
    // Nothing is in progress any more: the run that started last.
    QCOMPARE(pane->timeline_view()->time_window().first, pane->timeline().run_starts().back());
  }

  void theRunsFollowedAreRemembered() {
    using pychron::ui::TimelineView;
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    {
      ExperimentWindow window(bridge, true, settings());
      QCOMPARE(window.executor()->timeline_view()->follow_runs(), TimelineView::kFollowActive);  // nothing stored yet
      window.executor()->timeline_view()->context_actions()[3]->trigger();  // Follow last 12 runs
      QCOMPARE(window.executor()->timeline_view()->follow_runs(), 12);
    }
    ExperimentWindow again(bridge, true, settings());
    QCOMPARE(again.executor()->timeline_view()->follow_runs(), 12);
    QCOMPARE(again.executor()->timeline_view()->mode(), TimelineView::Mode::Follow);
  }

  // A long queue says more than the list should hold: the oldest lines go.
  void theEventsListKeepsItsNewestLines() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim), &error), qPrintable(error));
    auto* pane = window.executor();
    QCOMPARE(pychron::ui::ExecutorPane::kEventLimit, 5000);
    pane->set_event_limit(4);
    pane->request_start();
    QVERIFY(pane->running());
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    const QStringList events = pane->events();
    QCOMPARE(events.size(), 4);
    QVERIFY2(events.last().contains(QStringLiteral("queue completed")), qPrintable(events.join(QLatin1Char('\n'))));
    QVERIFY(!events.join(QLatin1Char('\n')).contains(QStringLiteral("run 0 ")));
    pane->set_event_limit(2);
    QCOMPARE(pane->events().size(), 2);
  }

  void aRunsLogShowsInTheExecutorPane() {
    if (!pychron::scripting::make_script_host()->available()) QSKIP("needs embedded Python to run laser_extract.py");
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(sim.dir / "queues" / "experiment.laser.toml", &error), qPrintable(error));
    auto* pane = window.executor();
    pane->request_start();
    QVERIFY(pane->running());
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    const QStringList events = pane->events();
    const auto index = [&](const QString& text) {
      for (int i = 0; i < events.size(); ++i)
        if (events[i].contains(text)) return i;
      return -1;
    };
    const int started = index(QStringLiteral("run 0 66001 started"));
    const int said = index(QStringLiteral("  66001: hole 3: centered, moved "));
    const int finished = index(QStringLiteral("run 0 66001-1: success"));
    QVERIFY2(said >= 0, qPrintable(events.join(QLatin1Char('\n'))));
    QVERIFY(started >= 0 && started < said);
    QVERIFY(said < finished);
    QVERIFY(index(QStringLiteral("  66001: hole 7: centered, moved ")) > finished);
  }

  // A camera the session cannot use is on show before any queue is started.
  void whatTheSessionCannotDoShowsInTheExecutorPane() {
    // the example's simulated camera over a laser that is real
    pychron::ui::test::SimLab sim({}, {}, [](std::string_view) { return false; });
    QCOMPARE(sim.session->problems().size(), std::size_t{1});
    const QString problem = QString::fromStdString(sim.session->problems().front());
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(contains(window.executor()->events(), problem));
    QVERIFY(window.executor()->error_text().contains(problem));
  }

  // A queue the database will not take does not start, and every line of the
  // reason is on show.
  void experimentStartRefusedWhilePeakCenterRuns() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim), &error), qPrintable(error));
    bool peak_center = true;
    window.set_peak_center_running([&peak_center] { return peak_center; });
    window.start();
    QVERIFY(!bridge.running());
    QCOMPARE(window.executor()->error_text(), QStringLiteral("peak center running in the spectrometer window"));
  }

  void aQueueThatCannotBeSavedSaysWhyInFull() {
    RefusingPersister persister;
    pychron::ui::test::SimLab sim({}, {}, {}, &persister);
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim), &error), qPrintable(error));
    window.start();
    QVERIFY(!bridge.running());
    QCOMPARE(window.executor()->error_text(),
             QStringLiteral("the queue cannot be saved:\nthe database cannot be reached: no such file\n"
                            "Run: elctl db migrate --db sqlite:/lab/data/pychron.db"));
  }

  // What is still to be saved is named for where it goes.
  void pendingSavesAreNamedForWhereTheyGo() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QCOMPARE(window.executor()->pending_text(2), QStringLiteral("2 record(s) in the spool"));
    bridge.set_saves_to_database(true);
    QCOMPARE(window.executor()->pending_text(2), QStringLiteral("2 run(s) waiting to be saved to the database"));
    // With the reason the last one was refused, on one line.
    QCOMPARE(window.executor()->pending_text(2, std::string("7f3a: identifier 'zz' is unknown\n  SQL: insert")),
             QStringLiteral("2 run(s) waiting to be saved to the database: 7f3a: identifier 'zz' is unknown SQL: insert"));
    bridge.set_saves_to_database(false);
    QCOMPARE(window.executor()->pending_text(1, std::string("7f3a: disk full")),
             QStringLiteral("1 record(s) in the spool: 7f3a: disk full"));
  }

  void withoutNotificationsATestSaysSo() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.executor()->notify_text().startsWith(QStringLiteral("off")));
    sim.session->notify_test();
    QTRY_VERIFY(contains(window.executor()->events(), QStringLiteral("no notifications are configured")));
  }

  void editsTheQueueWhileItRuns() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    auto& model = window.model();
    QStringList refused;
    connect(&model, &QueueTableModel::editRefused, this, [&](const QString& why) { refused.append(why); });
    bool skipped = false, moved = true;
    // As the first run starts (it runs for a while yet), the rows after it are open.
    connect(&bridge, &ExperimentBridge::runStarted, this, [&](const pychron::experiment::executor::RunStarted& e) {
      if (e.row != 0) return;
      model.set_frozen(1);  // the frontier event may not have arrived yet
      QVERIFY(!(model.flags(model.index(0, QueueTableModel::Identifier)) & Qt::ItemIsEditable));
      QVERIFY(model.flags(model.index(2, QueueTableModel::Identifier)) & Qt::ItemIsEditable);
      moved = model.move_up({1});  // would change the started row
      skipped = model.toggle_skip({2});
    });
    window.executor()->request_start();
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QVERIFY(!moved);
    QCOMPARE(refused.size(), 1);
    QVERIFY2(refused.front().contains(QStringLiteral("reached")), qPrintable(refused.front()));
    QVERIFY(skipped);
    QVERIFY(model.queue().runs[2].skip);  // the executor's echo did not undo it
    QVERIFY(model.version() >= 1);
    QVERIFY(window.modified());
    QCOMPARE(model.status(1), std::optional<RunState>(RunState::Success));
    QVERIFY(!model.status(2).has_value());
    QVERIFY(contains(window.executor()->events(), QStringLiteral("queue completed")));
    QCOMPARE(window.executor()->progress_text(), QStringLiteral("2/2 run(s)"));
  }

  void anOverlappedRunTakesASecondLane() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    auto queue = sim.queue();
    // The first unknown lets the next run start 30 s before it ends.
    queue.runs[1].overlap.duration = std::chrono::duration<double>(30);
    const fs::path file = sim.dir / "overlap.toml";
    QVERIFY(pychron::experiment::save_queue_file(file.string(), queue));
    QVERIFY(window.load_queue(file));
    window.executor()->request_start();
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QVERIFY(contains(window.executor()->events(), QStringLiteral("queue completed")));
    const auto& tl = window.executor()->timeline();
    QCOMPARE(tl.run_lanes(), 2);
    // Run 2 measured on lane 2 while run 1 (lane 1) was still in flight.
    std::optional<double> run1_end, run2_start;
    for (const auto& seg : tl.segments()) {
      if (!seg.state) continue;
      if (seg.lane == 1 && seg.label == QStringLiteral("66001")) run1_end = std::max(run1_end.value_or(0), *seg.end);
      if (seg.lane == 2 && !run2_start) run2_start = seg.start;
    }
    QVERIFY(run1_end && run2_start);
    QVERIFY2(*run2_start < *run1_end, qPrintable(QStringLiteral("%1 %2").arg(*run2_start).arg(*run1_end)));
  }

  void cancelAsksFirstAndStartsAtTheFirstNotExecutedRow() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, false, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(!window.windowTitle().contains(QStringLiteral("Simulation")));
    int asked = 0;
    bool answer = false;
    window.set_confirm([&](const QString&, const QString&) {
      ++asked;
      return answer;
    });
    window.select_row(1);
    window.start();  // the selection is not read: the first row not executed
    QTRY_VERIFY_WITH_TIMEOUT(window.model().status(0).has_value(), 30000);

    window.executor()->request_cancel();  // declined
    window.executor()->request_abort();   // declined
    QCOMPARE(asked, 2);
    QVERIFY(!contains(window.executor()->events(), QStringLiteral("requested")));
    QVERIFY(bridge.running());

    answer = true;
    window.executor()->request_cancel();
    QCOMPARE(asked, 3);
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 30000);
    QVERIFY(contains(window.executor()->events(), QStringLiteral("cancel requested")));
    QVERIFY(contains(window.executor()->events(), QStringLiteral("queue cancelled")));
    QCOMPARE(window.model().status(0), std::optional<RunState>(RunState::Cancelled));
    QVERIFY(!window.model().status(1).has_value());
  }

  // ---- the Executed and Queue tables (queue-tables design 3) -------------------

  void theExecutedSectionShowsOnlyWithExecutedRows() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(!window.executed_section()->isVisible());
    QVERIFY(window.model().resume_from(1, {}));
    QVERIFY(window.executed_section()->isVisible());
    auto* count = window.findChild<QLabel*>(QStringLiteral("executed_count"));
    QVERIFY(count != nullptr);
    QCOMPARE(count->text(), QStringLiteral("Executed (1)"));
    QCOMPARE(window.executed_table()->model()->rowCount(), 1);
    QCOMPARE(window.queue_table()->model()->rowCount(), 2);
    QVERIFY(window.model().reset_executed());
    QVERIFY(!window.executed_section()->isVisible());
  }

  void theTablesKeepTheirFoldAndSplitAcrossAReopen() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    QByteArray split;
    {
      ExperimentWindow window(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
      QVERIFY(window.load_queue(queue_file(sim)));
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      QVERIFY(window.model().resume_from(1, {}));
      QVERIFY(!window.executed_collapsed());
      window.set_executed_collapsed(true);
      QVERIFY(window.executed_collapsed());
      QVERIFY(!window.executed_table()->isVisible());  // the header only
      auto* fold = window.findChild<QToolButton*>(QStringLiteral("executed_fold"));
      QVERIFY(fold != nullptr && fold->isVisible());
      window.tables_splitter()->setSizes({100, 400});
      split = window.tables_splitter()->saveState();
      QVERIFY(window.close());
    }
    ExperimentWindow again(bridge, true, std::make_unique<QSettings>(file, QSettings::IniFormat));
    QVERIFY(again.executed_collapsed());
    QCOMPARE(again.tables_splitter()->saveState(), split);
  }

  void bothTablesShareTheirColumnWidths() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    window.queue_table()->setColumnWidth(QueueTableModel::Comment, 222);
    QCOMPARE(window.executed_table()->columnWidth(QueueTableModel::Comment), 222);
    QCOMPARE(window.executed_table()->columnWidth(QueueTableModel::Row), window.queue_table()->columnWidth(QueueTableModel::Row));
  }

  void rowActionsUseTheQueueSelection() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.model().resume_from(1, {}));
    const auto executed = window.model().queue().runs[0];
    const auto third = window.model().queue().runs[2];
    auto* queue = window.queue_table();
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(queue->model());
    QVERIFY(proxy != nullptr);
    QCOMPARE(proxy->mapToSource(proxy->index(0, 0)).row(), 1);
    window.select_row(0);  // executed: not in the Queue view
    QVERIFY(queue->selectionModel()->selectedRows().isEmpty());
    window.select_row(1);
    QCOMPARE(queue->selectionModel()->selectedRows().size(), 1);
    QAction* remove = nullptr;
    for (QAction* a : queue->actions())
      if (a->text() == QStringLiteral("Delete")) remove = a;
    QVERIFY(remove != nullptr);
    // The window's actions are enabled while it is the active window (MenuHub).
    window.show();
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window)) QSKIP("this platform does not activate windows");
    QVERIFY(remove->isEnabled());
    remove->trigger();
    QCOMPARE(window.model().rowCount(), 2);
    QVERIFY(window.model().queue().runs[0] == executed);
    QVERIFY(window.model().queue().runs[1] == third);
  }

  void startBeginsAtTheFirstNotExecutedRow() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.model().resume_from(1, {}));
    window.select_row(2);
    window.start();
    QVERIFY(bridge.running());
    QTRY_VERIFY_WITH_TIMEOUT(window.model().status(1).has_value(), 30000);
    QVERIFY(!window.model().status(0).has_value());  // the resume gave it none
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QCOMPARE(window.model().status(1), std::optional<RunState>(RunState::Success));
    QCOMPARE(window.model().status(2), std::optional<RunState>(RunState::Success));
    QCOMPARE(window.model().executed_rows(), 3u);
  }

  // ---- Queue > Reset, Clear Executed, Repeat Last Run (queue-tables design 4) ----

  void resetAndClearExecutedAreRefusedWhileRunning() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    int asked = 0;
    window.set_ask_clear_executed([&](int) {
      ++asked;
      return true;
    });
    window.start();
    QVERIFY(bridge.running());
    QString error;
    QVERIFY(!window.reset_executed(&error));
    QCOMPARE(error, QStringLiteral("a queue is running"));
    error.clear();
    QVERIFY(!window.clear_executed(&error));
    QCOMPARE(error, QStringLiteral("a queue is running"));
    QCOMPARE(asked, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QCOMPARE(window.model().rowCount(), 3);
  }

  void clearExecutedAsksRemovesAndMarksModified() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    const auto last = window.model().queue().runs[2];
    QVERIFY(window.model().resume_from(2, {}));
    int asked_for = -1;
    bool answer = false;
    window.set_ask_clear_executed([&](int runs) {
      asked_for = runs;
      return answer;
    });
    QString error;
    QVERIFY(!window.clear_executed(&error));
    QCOMPARE(error, QStringLiteral("cancelled"));
    QCOMPARE(asked_for, 2);
    QCOMPARE(window.model().rowCount(), 3);
    QVERIFY(!window.modified());

    answer = true;
    asked_for = -1;
    QVERIFY(window.clear_executed(&error));
    QCOMPARE(asked_for, 2);
    QCOMPARE(window.model().rowCount(), 1);
    QVERIFY(window.model().queue().runs[0] == last);
    QCOMPARE(window.model().executed_rows(), 0u);
    QVERIFY(window.modified());

    error.clear();
    QVERIFY(!window.clear_executed(&error));  // nothing executed now
    QCOMPARE(error, QStringLiteral("no run has been executed"));
  }

  void resetPutsTheRunsBackWithoutModifying() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.model().resume_from(2, {{0, RunState::Success}}));
    QCOMPARE(window.queue_table()->model()->rowCount(), 1);
    QVERIFY(window.reset_executed());
    QCOMPARE(window.model().executed_rows(), 0u);
    QVERIFY(!window.model().status(0).has_value());
    QVERIFY(!window.modified());
    QCOMPARE(window.queue_table()->model()->rowCount(), 3);
  }

  void repeatInsertsAtTheFrontOfTheQueue() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    auto& model = window.model();
    const auto first = model.queue().runs[0].id;
    const auto second = model.queue().runs[1].id;
    auto identifier = [&](int row) {
      return model.index(row, QueueTableModel::Identifier).data().toString();
    };
    QVERIFY(model.resume_from(2, {}));
    const QString second_shown = identifier(1);

    // Nothing selected in Executed: its last row.
    QString error;
    QVERIFY2(window.repeat_last(&error), qPrintable(error));
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.queue().runs[2].id.identifier, second.identifier);
    QVERIFY(!model.queue().runs[2].id.aliquot.has_value());
    auto* queue = window.queue_table()->model();
    QCOMPARE(queue->index(0, QueueTableModel::Identifier).data().toString(), second_shown);
    QVERIFY(window.modified());

    // The rows selected in Executed.
    auto* executed = window.executed_table();
    executed->selectionModel()->select(executed->model()->index(0, 0),
                                       QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY2(window.repeat_last(&error), qPrintable(error));
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(model.queue().runs[2].id.identifier, first.identifier);
    QCOMPARE(model.queue().runs[3].id.identifier, second.identifier);

    // While running: an ordinary live edit at the front of the Queue table.
    window.set_ask_unsaved([] { return ExperimentWindow::Unsaved::Discard; });
    QVERIFY2(window.load_queue(queue_file(sim, "again.toml"), &error), qPrintable(error));
    window.start();
    QVERIFY(bridge.running());
    QTRY_VERIFY_WITH_TIMEOUT(model.executed_rows() >= 1, 30000);
    QVERIFY2(window.repeat_last(&error), qPrintable(error));
    QCOMPARE(model.rowCount(), 4);
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QVERIFY(contains(window.executor()->events(), QStringLiteral("queue completed")));
  }

  void repeatWithNothingExecutedSaysSo() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    window.select_row(1);  // a Queue row is never what is repeated
    QString error;
    QVERIFY(!window.repeat_last(&error));
    QCOMPARE(error, QStringLiteral("no run has been executed"));
    QCOMPARE(window.model().rowCount(), 3);
    QVERIFY(!window.modified());
  }

  void theExecutedTablesMenuRepeatsAndOpensScripts() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.model().resume_from(2, {}));
    auto* executed = window.executed_table();
    QCOMPARE(executed->contextMenuPolicy(), Qt::ActionsContextMenu);
    auto action = [&](const QString& text) -> QAction* {
      for (QAction* a : executed->actions())
        if (a->text() == text) return a;
      return nullptr;
    };
    QAction* repeat = action(QStringLiteral("Repeat"));
    QAction* extraction = action(QStringLiteral("Open Extraction Script"));
    QAction* post = action(QStringLiteral("Open Post-Measurement Script"));
    QVERIFY(repeat != nullptr && extraction != nullptr && post != nullptr);

    executed->selectionModel()->select(executed->model()->index(1, 0),
                                       QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const bool has_script = !window.model().queue().runs[1].extraction.script.empty();
    extraction->trigger();
    QCOMPARE(window.script_editor() != nullptr, has_script);
    if (has_script) {
      QCOMPARE(window.script_editor()->current_name(), QStringLiteral("extraction/sim_extract"));
      post->trigger();
      QCOMPARE(window.script_editor()->current_name(), QStringLiteral("post_measurement/sim_pump"));
    }
    repeat->trigger();
    QCOMPARE(window.model().rowCount(), 4);
    QCOMPARE(window.model().queue().runs[2].id.identifier, window.model().queue().runs[1].id.identifier);
  }

  // ---- resume after a crash or a Stop (queue-tables design 5) ----------------

  void resumeStartsAtTheFilesNextRowWithEarlierStatuses() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    write_example_state(sim, 1);
    int asked = 0;
    window.set_ask_resume([&](const pychron::ui::ResumeOffer& offer) {
      ++asked;
      return offer.resumable();
    });
    QString error;
    QVERIFY2(window.offer_resume(&error), qPrintable(error));
    QCOMPARE(asked, 1);
    auto& model = window.model();
    QCOMPARE(model.status(0), std::optional<RunState>(RunState::Success));
    QVERIFY(model.executed_rows() >= 1);
    QVERIFY(bridge.running());
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QCOMPARE(model.status(0), std::optional<RunState>(RunState::Success));
    QCOMPARE(model.status(1), std::optional<RunState>(RunState::Success));
    QCOMPARE(model.status(2), std::optional<RunState>(RunState::Success));
  }

  void notNowChangesNothing() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    write_example_state(sim, 1);
    int asked = 0;
    window.set_ask_resume([&](const pychron::ui::ResumeOffer&) {
      ++asked;
      return false;
    });
    QString error;
    QVERIFY(!window.offer_resume(&error));
    QVERIFY(error.isEmpty());
    QCOMPARE(asked, 1);
    QCOMPARE(window.model().executed_rows(), std::size_t{0});
    QVERIFY(!window.model().status(0).has_value());
    QVERIFY(!bridge.running());
  }

  void noOfferWithoutAGoodFile() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    int asked = 0;
    window.set_ask_resume([&](const pychron::ui::ResumeOffer&) {
      ++asked;
      return true;
    });
    const fs::path file = sim.dir / "data" / "executor_state.json";
    fs::remove(file);
    QVERIFY(!window.offer_resume());  // no file
    write_state(sim, "not json");
    QVERIFY(!window.offer_resume());
    write_state(sim, R"({"queue": ")" + sim.queue().name + R"("})");  // no next_row
    QVERIFY(!window.offer_resume());
    write_example_state(sim, 3);  // the row count: a queue shortened since, or one that ended
    QVERIFY(!window.offer_resume());
    QCOMPARE(asked, 0);
    QCOMPARE(window.model().executed_rows(), std::size_t{0});
    QVERIFY(!bridge.running());
  }

  void noOfferInTheWindowThatJustStopped() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    const fs::path queue = queue_file(sim);
    QVERIFY(window.load_queue(queue));
    int asked = 0;
    window.set_ask_resume([&](const pychron::ui::ResumeOffer&) {
      ++asked;
      return false;
    });
    window.start();
    QVERIFY(bridge.running());
    QTRY_VERIFY_WITH_TIMEOUT(window.model().status(0).has_value(), 30000);
    bridge.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QVERIFY(window.model().executed_rows() > 0);
    QVERIFY(window.model().executed_rows() < 3);
    QVERIFY(!window.offer_resume());
    QCOMPARE(asked, 0);

    // Opened again, the same queue is offered from where it stopped.
    QVERIFY(window.load_queue(queue));
    QVERIFY(!window.offer_resume());
    QCOMPARE(asked, 1);
  }

  void anOfferWithAnotherIdentifierCannotResume() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    write_example_state(sim, 1, "zzz");
    bool resumable = true;
    window.set_ask_resume([&](const pychron::ui::ResumeOffer& offer) {
      resumable = offer.resumable();
      return true;
    });
    QVERIFY(!window.offer_resume());
    QVERIFY(!resumable);
    QVERIFY(!bridge.running());
    QCOMPARE(window.model().executed_rows(), std::size_t{0});
  }

  void resumeIsRefusedWhileRunningAndSaysSo() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    const fs::path file = write_example_state(sim, 1);
    int asked = 0;
    window.set_ask_resume([&](const pychron::ui::ResumeOffer&) {
      ++asked;
      return false;
    });
    QStringList told;
    window.set_inform([&](const QString& text) { told.append(text); });
    QVERIFY(window.resume_action() != nullptr);
    // The window's actions are enabled while it is the active window (MenuHub).
    window.show();
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window)) QSKIP("this platform does not activate windows");
    QVERIFY(window.resume_action()->isEnabled());

    window.start();
    QVERIFY(bridge.running());
    QString error;
    QVERIFY(!window.offer_resume(&error));
    QCOMPARE(error, QStringLiteral("a queue is running"));
    window.resume_action()->trigger();
    QCOMPARE(told, QStringList{QStringLiteral("A queue is running.")});
    QCOMPARE(asked, 0);

    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QVERIFY(window.reset_executed());
    fs::remove(file);
    told.clear();
    window.resume_action()->trigger();
    QCOMPARE(told, QStringList{QStringLiteral("Nothing to resume.")});
    QCOMPARE(asked, 0);
  }

  void aQueueWithErrorsCannotStart() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.model().setData(window.model().index(0, QueueTableModel::Plan), QStringLiteral("no_such_plan")));
    QVERIFY(window.modified());
    QVERIFY(window.windowTitle().endsWith(QStringLiteral("*")));
    QVERIFY(!window.executor()->start_enabled());
    window.start();  // the window itself still refuses
    QVERIFY(!bridge.running());
    QVERIFY(window.executor()->error_text().contains(QStringLiteral("runs[0]")));

    // A queue-level problem is shown above the table.
    QVERIFY(window.queue_diagnostics_text().isEmpty());
    pychron::experiment::QueueSpec bad = window.model().queue();
    bad.queue_conditionals = "no_such_set";
    window.model().set_queue(bad);
    QVERIFY(window.queue_diagnostics_text().contains(QStringLiteral("queue.queue_conditionals: unknown queue conditionals")));
  }

  void savingRoundTripsAndUnsavedEditsAsk() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    auto window = std::make_unique<ExperimentWindow>(bridge, true, settings());
    QVERIFY(window->load_queue(queue_file(sim)));
    QString error;
    QVERIFY(!(ExperimentWindow(bridge, true, settings()).save(&error)));  // no file yet
    QVERIFY(window->model().setData(window->model().index(1, QueueTableModel::Comment), QStringLiteral("edited")));
    QVERIFY(window->model().duplicate({2}));
    QVERIFY(window->modified());

    const fs::path out = sim.dir / "saved.toml";
    QVERIFY2(window->save_as(out, &error), qPrintable(error));
    QVERIFY(!window->modified());
    auto back = pychron::experiment::load_queue_file(out.string(), sim.lab.ids);
    QVERIFY(back.has_value());
    QVERIFY(*back == window->model().queue());
    QCOMPARE(back->runs.size(), std::size_t{4});
    QCOMPARE(back->runs[1].comment, std::string("edited"));

    // Unsaved edits: Cancel keeps the window (and the queue), Discard lets go.
    QVERIFY(window->model().toggle_skip({0}));
    window->show();
    int asked = 0;
    window->set_ask_unsaved([&] {
      ++asked;
      return ExperimentWindow::Unsaved::Cancel;
    });
    QVERIFY(!window->close());
    QVERIFY(!window->load_queue(out, &error));
    QCOMPARE(asked, 2);
    QVERIFY(window->model().queue().runs[0].skip);
    window->set_ask_unsaved([] { return ExperimentWindow::Unsaved::Discard; });
    QVERIFY(window->load_queue(out));
    QVERIFY(!window->model().queue().runs[0].skip);
    QVERIFY(!window->modified());
    window.reset();
  }

  // ---- File > Open Recent -------------------------------------------------------

  void openRecentListsOpenedAndSavedQueues() {
    pychron::ui::test::SimLab sim;
    const QTemporaryDir own;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings_in(own));
    QString error;
    QVERIFY2(window.load_queue(queue_file(sim, "a.toml"), &error), qPrintable(error));
    QVERIFY2(window.save_as(sim.dir / "b.toml", &error), qPrintable(error));
    QMenu* menu = window.recent_menu();
    QVERIFY(menu != nullptr);
    emit menu->aboutToShow();
    const QList<QAction*> actions = menu->actions();
    QCOMPARE(actions.size(), 4);
    QCOMPARE(actions.at(0)->text(), QStringLiteral("b.toml"));
    QCOMPARE(actions.at(1)->text(), QStringLiteral("a.toml"));
    QCOMPARE(actions.at(0)->data().toString(), canonical(sim.dir / "b.toml"));
    QCOMPARE(actions.at(0)->toolTip(), canonical(sim.dir / "b.toml"));
    QVERIFY(actions.at(2)->isSeparator());
    QCOMPARE(actions.at(3)->text(), QStringLiteral("Clear Menu"));
    // Window scope: enabled only while the window is in front.
    if (!shown_active(window)) QSKIP("this platform does not activate windows");
    QVERIFY(menu->menuAction()->isEnabled());
  }

  void openRecentIsDisabledWhenEmpty() {
    pychron::ui::test::SimLab sim;
    const QTemporaryDir own;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings_in(own));
    QVERIFY(window.recent_menu() != nullptr);
    QVERIFY(!window.recent_menu()->menuAction()->isEnabled());
    if (!shown_active(window)) QSKIP("this platform does not activate windows");
    QVERIFY(!window.recent_menu()->menuAction()->isEnabled());
    // In front, with something to open, it is.
    QVERIFY(window.load_queue(queue_file(sim, "a.toml")));
    QVERIFY(window.recent_menu()->menuAction()->isEnabled());
  }

  void choosingAnEntryOpensIt() {
    pychron::ui::test::SimLab sim;
    const QTemporaryDir own;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings_in(own));
    QVERIFY(window.load_queue(queue_file(sim, "a.toml")));
    QVERIFY(window.load_queue(queue_file(sim, "b.toml")));
    QVERIFY(window.path()->filename() == "b.toml");
    QAction* a = recent_entry(window, QStringLiteral("a.toml"));
    QVERIFY(a != nullptr);
    a->trigger();
    QVERIFY(window.path().has_value());
    QVERIFY(window.path()->filename() == "a.toml");
  }

  void clearMenuEmptiesIt() {
    pychron::ui::test::SimLab sim;
    const QTemporaryDir own;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    auto settings = settings_in(own);
    const QString file = settings->fileName();
    ExperimentWindow window(bridge, true, std::move(settings));
    QVERIFY(window.load_queue(queue_file(sim, "a.toml")));
    QAction* clear = recent_entry(window, QStringLiteral("Clear Menu"));
    QVERIFY(clear != nullptr);
    clear->trigger();
    QVERIFY(!window.recent_menu()->menuAction()->isEnabled());  // whether in front or not
    const QSettings stored(file, QSettings::IniFormat);
    QVERIFY(stored.value(QStringLiteral("experiment_window/recent_queues")).toStringList().isEmpty());
  }

  void anEntryAsksAboutUnsavedEdits() {
    pychron::ui::test::SimLab sim;
    const QTemporaryDir own;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings_in(own));
    QVERIFY(window.load_queue(queue_file(sim, "a.toml")));
    QVERIFY(window.load_queue(queue_file(sim, "b.toml")));
    QVERIFY(window.model().toggle_skip({0}));
    QVERIFY(window.modified());
    int asked = 0;
    window.set_ask_unsaved([&] {
      ++asked;
      return ExperimentWindow::Unsaved::Cancel;
    });
    QAction* a = recent_entry(window, QStringLiteral("a.toml"));
    QVERIFY(a != nullptr);
    a->trigger();
    QCOMPARE(asked, 1);
    QVERIFY(window.path()->filename() == "b.toml");
    QVERIFY(window.modified());
    window.set_ask_unsaved([] { return ExperimentWindow::Unsaved::Discard; });
  }

  // File > New: an empty queue with no file, after the usual question.
  void aNewQueueIsEmptyAndHasNoFile() {
    using pychron::ui::MenuHub;
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    const std::size_t rows = window.model().queue().runs.size();
    QVERIFY(rows > 0);
    QString error;
    QVERIFY2(window.new_queue(&error), qPrintable(error));
    QVERIFY(window.model().queue().runs.empty());
    QVERIFY(!window.path());
    QVERIFY(!window.modified());
    QVERIFY(!window.save(&error));  // no file to save to

    // Unsaved edits: Cancel keeps the queue and its file, Discard lets go.
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.model().toggle_skip({0}));
    int asked = 0;
    auto answer = ExperimentWindow::Unsaved::Cancel;
    window.set_ask_unsaved([&] {
      ++asked;
      return answer;
    });
    QVERIFY(!window.new_queue(&error));
    QCOMPARE(error, QStringLiteral("cancelled"));
    QCOMPARE(asked, 1);
    QCOMPARE(window.model().queue().runs.size(), rows);
    QVERIFY(window.path().has_value());
    QVERIFY(window.modified());
    answer = ExperimentWindow::Unsaved::Discard;
    QVERIFY(window.new_queue(&error));
    QVERIFY(window.model().queue().runs.empty());
    QVERIFY(!window.path());

  }

  void aQueueCannotBeReplacedByANewOneWhileItRuns() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    const std::size_t rows = window.model().queue().runs.size();
    window.executor()->request_start();
    QVERIFY(window.executor()->running());
    QString error;
    QVERIFY(!window.new_queue(&error));
    QCOMPARE(error, QStringLiteral("a queue is running"));
    QCOMPARE(window.model().queue().runs.size(), rows);
    QVERIFY(window.path().has_value());
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
  }

  // The identifier source reaches the run factory whichever of the two comes first.
  void theMainWindowHandsTheSourceToTheFactory() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    OnePackageSource source;
    main.set_identifier_source(&source);
    main.set_experiment(&bridge, true, queue_file(sim), [this] { return settings(); });
    main.experiment_action()->trigger();
    QVERIFY(main.experiment_window() != nullptr);
    QCOMPARE(main.experiment_window()->factory()->package_choices().size(), 2);
    main.set_experiment(nullptr, false);
  }

  void aSourceSetLaterReachesAnOpenWindow() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    main.set_experiment(&bridge, true, queue_file(sim), [this] { return settings(); });
    main.experiment_action()->trigger();
    QVERIFY(main.experiment_window() != nullptr);
    QCOMPARE(main.experiment_window()->factory()->package_choices().size(), 1);
    OnePackageSource source;
    main.set_identifier_source(&source);
    QCOMPARE(main.experiment_window()->factory()->package_choices().size(), 2);
    main.set_experiment(nullptr, false);
  }

  // File > New and File > Open are the main window's, from any window: each
  // brings up the window it acts in, and has the key while that one is in front.
  void fileNewAndOpenBringUpTheWindowTheyActIn() {
    using pychron::ui::MenuHub;
    using pychron::ui::Shortcut;
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    main.show();
    const QList<QAction*>& entries = main.file_entries();
    QStringList texts;
    for (const QAction* a : entries) texts << a->text();
    QCOMPARE(texts, (QStringList{QStringLiteral("New &Queue"), QStringLiteral("New &Script…"),
                                 QStringLiteral("New &Conditionals…"), QStringLiteral("Open &Queue…"),
                                 QStringLiteral("Open &Script…"), QStringLiteral("Open &Conditionals…")}));
    MenuHub& hub = MenuHub::instance();
    QCOMPARE(hub.file_entries(MenuHub::FileList::New), entries.mid(0, 3));
    QCOMPARE(hub.file_entries(MenuHub::FileList::Open), entries.mid(3, 3));
    for (const QAction* a : entries) QVERIFY2(!a->isEnabled(), qPrintable(a->text()));  // no session yet
    entries.at(0)->trigger();  // and nothing to bring up
    QVERIFY(main.experiment_window() == nullptr);

    main.set_experiment(&bridge, true, queue_file(sim), [this] { return settings(); });
    for (const QAction* a : entries) QVERIFY2(a->isEnabled(), qPrintable(a->text()));

    // Open Script…: the script editor alone, made with the experiment window, which stays down.
    // (the picker is a dialog until the editor is there to be told otherwise: dismissed)
    QTimer::singleShot(0, this, [] {
      if (QWidget* dialog = QApplication::activeModalWidget()) dialog->close();
    });
    entries.at(4)->trigger();
    ExperimentWindow* experiment = main.experiment_window();
    QVERIFY(experiment != nullptr);
    QVERIFY(!experiment->isVisible());
    QVERIFY(experiment->model().rowCount() > 0);  // with the queue it was to start with
    auto* scripts = experiment->script_editor();
    QVERIFY(scripts != nullptr);
    QVERIFY(scripts->isVisible());
    QCOMPARE(scripts->document_count(), 0);
    scripts->set_pick_script([](const QStringList& names) { return std::optional<QString>(names.first()); });
    entries.at(4)->trigger();
    QCOMPARE(scripts->document_count(), 1);

    // Open Conditionals…: its editor.
    auto* conditionals = experiment->open_conditionals_editor();
    conditionals->hide();
    conditionals->set_pick_file([](const QStringList& names) { return std::optional<QString>(names.last()); });
    entries.at(5)->trigger();
    QVERIFY(conditionals->isVisible());
    QCOMPARE(conditionals->current_name(), conditionals->file_names().last());

    // New Queue: the experiment window, emptied.
    entries.at(0)->trigger();
    QVERIFY(experiment->isVisible());
    QVERIFY(experiment->model().queue().runs.empty());
    QVERIFY(!experiment->path());

    // The keys follow the window in front.
    experiment->activateWindow();
    if (!QTest::qWaitForWindowActive(experiment)) QSKIP("this platform does not activate windows");
    QCOMPARE(entries.at(0)->shortcut(), pychron::ui::key(Shortcut::FileNew));
    QCOMPARE(entries.at(3)->shortcut(), pychron::ui::key(Shortcut::FileOpen));
    QVERIFY(entries.at(4)->shortcut().isEmpty());
    QCOMPARE(hub.file_action(MenuHub::FileRole::Save)->text(), QStringLiteral("&Save Queue"));
    scripts->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), static_cast<QWidget*>(scripts));
    QCOMPARE(entries.at(1)->shortcut(), pychron::ui::key(Shortcut::FileNew));
    QCOMPARE(entries.at(4)->shortcut(), pychron::ui::key(Shortcut::FileOpen));
    QVERIFY(entries.at(3)->shortcut().isEmpty());
    conditionals->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), static_cast<QWidget*>(conditionals));
    QCOMPARE(entries.at(2)->shortcut(), pychron::ui::key(Shortcut::FileNew));
    QCOMPARE(entries.at(5)->shortcut(), pychron::ui::key(Shortcut::FileOpen));
    main.activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), static_cast<QWidget*>(&main));
    for (const QAction* a : entries) QVERIFY2(a->shortcut().isEmpty(), qPrintable(a->text()));

    main.set_experiment(nullptr, false);
    for (const QAction* a : entries) QVERIFY2(!a->isEnabled(), qPrintable(a->text()));
  }

  void rowsOpenTheirScriptsInTheEditor() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.script_editor() == nullptr);
    QVERIFY(!window.edit_row_script(pychron::scripting::ScriptKind::Extraction));  // nothing selected
    window.select_row(1);
    const bool has_script = !window.model().queue().runs[1].extraction.script.empty();
    QCOMPARE(window.edit_row_script(pychron::scripting::ScriptKind::Extraction), has_script);
    if (has_script) {
      QVERIFY(window.script_editor() != nullptr);
      QCOMPARE(window.script_editor()->current_name(), QStringLiteral("extraction/sim_extract"));
      QVERIFY(window.edit_row_script(pychron::scripting::ScriptKind::PostMeasurement));
      QCOMPARE(window.script_editor()->current_name(), QStringLiteral("post_measurement/sim_pump"));
      QCOMPARE(window.script_editor()->document_count(), 2);
    }
    // A new script the queue names makes its row valid.
    auto run = window.model().queue().runs[1];
    run.extraction.script = "brand_new";
    QVERIFY(window.model().replace_run(1, run));
    QVERIFY(window.model().row_has_error(1));
    QString error;
    QVERIFY2(window.open_script_editor()->new_script(pychron::scripting::ScriptKind::Extraction,
                                                    QStringLiteral("brand_new"), &error),
             qPrintable(error));
    QVERIFY(!window.model().row_has_error(1));
  }

  void queueConditionalsAreChosenFromTheLabsFiles() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QComboBox* combo = window.queue_conditionals_combo();
    QStringList items;
    for (int i = 0; i < combo->count(); ++i) items.append(combo->itemText(i));
    QCOMPARE(items, (QStringList{QStringLiteral("(none)"), QStringLiteral("system"), QStringLiteral("default_unknown")}));
    QCOMPARE(combo->currentIndex(), 0);
    QVERIFY(!window.modified());

    combo->setCurrentIndex(2);
    emit combo->activated(2);
    QCOMPARE(window.model().queue().queue_conditionals, std::string("default_unknown"));
    QVERIFY(window.modified());

    // It goes to the file and comes back selected.
    const fs::path out = sim.dir / "with_conditionals.toml";
    QVERIFY(window.save_as(out));
    combo->setCurrentIndex(0);
    emit combo->activated(0);
    QVERIFY(window.model().queue().queue_conditionals.empty());
    window.set_ask_unsaved([] { return ExperimentWindow::Unsaved::Discard; });
    QVERIFY(window.load_queue(out));
    QCOMPARE(combo->currentText(), QStringLiteral("default_unknown"));

    // A queue naming a file the lab does not have: shown, not dropped.
    pychron::experiment::QueueSpec bad = window.model().queue();
    bad.queue_conditionals = "gone";
    window.model().set_queue(bad);
    QCOMPARE(combo->currentText(), QStringLiteral("gone"));
    QCOMPARE(combo->count(), 4);
    QCOMPARE(window.model().queue().queue_conditionals, std::string("gone"));
  }

  void rowsTakeConditionalsFromADialog() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    using States = QList<Qt::CheckState>;
    QStringList offered;
    States initial;
    std::optional<States> answer;
    int asked = 0;
    window.set_pick_conditionals([&](const QStringList& names, const States& states) {
      ++asked;
      offered = names;
      initial = states;
      return answer;
    });
    QVERIFY(!window.edit_selected_conditionals());  // nothing selected
    QCOMPARE(asked, 0);

    // One row has a file (and one the lab lacks), the other has none.
    auto run = window.model().queue().runs[0];
    run.conditionals = {{"default_unknown", "truncate"}, {"gone", "action"}};
    QVERIFY(window.model().replace_run(0, run));
    window.select_row(0);
    window.queue_table()->selectionModel()->select(window.queue_table()->model()->index(1, 0),
                                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    answer = std::nullopt;  // cancelled
    QVERIFY(!window.edit_selected_conditionals());
    QCOMPARE(asked, 1);
    QCOMPARE(offered, (QStringList{QStringLiteral("system"), QStringLiteral("default_unknown"), QStringLiteral("gone")}));
    QCOMPARE(initial, (States{Qt::Unchecked, Qt::PartiallyChecked, Qt::PartiallyChecked}));
    QCOMPARE(window.model().queue().runs[0].conditionals.size(), std::size_t{2});

    // OK without touching anything changes nothing.
    answer = initial;
    QVERIFY(window.edit_selected_conditionals());
    QCOMPARE(window.model().queue().runs[0].conditionals, run.conditionals);
    QVERIFY(window.model().queue().runs[1].conditionals.empty());

    // system for both; default_unknown left as each row had it; gone removed.
    answer = States{Qt::Checked, Qt::PartiallyChecked, Qt::Unchecked};
    QVERIFY(window.edit_selected_conditionals());
    using Ref = pychron::experiment::ConditionalRef;
    QCOMPARE(window.model().queue().runs[0].conditionals,
             (std::vector<Ref>{{"default_unknown", "truncate"}, {"system", "action"}}));
    QCOMPARE(window.model().queue().runs[1].conditionals, (std::vector<Ref>{{"system", "action"}}));
    QVERIFY(window.model().queue().runs[2].conditionals.empty());
    QVERIFY(window.modified());
    QCOMPARE(window.model().data(window.model().index(0, QueueTableModel::Conditionals)).toString(),
             QStringLiteral("default_unknown, system"));
  }

  void theConditionalsEditorOpensAndItsSavesRevalidate() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.conditionals_editor() == nullptr);
    auto* editor = window.open_conditionals_editor(QStringLiteral("default_unknown"));
    QVERIFY(editor != nullptr);
    QCOMPARE(editor->current_name(), QStringLiteral("default_unknown"));
    QCOMPARE(window.open_conditionals_editor(), editor);
    QCOMPARE(editor->current_name(), QStringLiteral("default_unknown"));  // no file named: stays

    // A run naming a file that does not exist yet; making it in the editor fixes the row.
    QVERIFY(window.model().set_conditionals({1}, {"run_x"}));
    QVERIFY(window.model().row_has_error(1));
    QVERIFY(editor->new_file(QStringLiteral("run_x")));
    QString error;
    QVERIFY2(editor->save(&error), qPrintable(error));
    QVERIFY(!window.model().row_has_error(1));
    QVERIFY(window.queue_conditionals_combo()->findText(QStringLiteral("run_x")) >= 0);
    QVERIFY(window.factory()->conditional_choices().contains(QStringLiteral("run_x")));

    // Deleting says the open queue uses it; once gone the row is an error again.
    QString asked;
    editor->set_confirm([&](const QString& q) {
      asked = q;
      return true;
    });
    QVERIFY(editor->delete_file(QStringLiteral("run_x")));
    QVERIFY2(asked.contains(QStringLiteral("queue")), qPrintable(asked));
    QVERIFY(window.model().row_has_error(1));
    QVERIFY(window.queue_conditionals_combo()->findText(QStringLiteral("run_x")) < 0);
    editor->close();
  }

  void setConditionalsIsOneEditAndGivesUpIfTheQueueChanged() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    using States = QList<Qt::CheckState>;
    QVERIFY(window.model().set_conditionals({0}, {"default_unknown"}));
    window.queue_table()->selectAll();
    int edits = 0;
    connect(&window.model(), &QueueTableModel::edited, this, [&] { ++edits; });
    // Rows end up with different lists (row 0 keeps its own file too): still one edit.
    window.set_pick_conditionals([](const QStringList&, const States&) {
      return std::optional<States>(States{Qt::Checked, Qt::PartiallyChecked});
    });
    QVERIFY(window.edit_selected_conditionals());
    QCOMPARE(edits, 1);
    QCOMPARE(window.model().queue().runs[0].conditionals.size(), std::size_t{2});
    QCOMPARE(window.model().queue().runs[2].conditionals.size(), std::size_t{1});

    // The queue changes while the dialog is up (the executor inserts or removes runs):
    // the answer was about other rows, so nothing is applied.
    window.queue_table()->selectAll();
    window.set_pick_conditionals([&](const QStringList&, const States& states) {
      [&] { QVERIFY(window.model().remove({0, 1})); }();
      return std::optional<States>(States(states.size(), Qt::Unchecked));
    });
    QVERIFY(!window.edit_selected_conditionals());
    QCOMPARE(window.model().rowCount(), 1);
    QCOMPARE(window.model().queue().runs[0].conditionals.size(), std::size_t{1});
    QVERIFY(!window.executor()->error_text().isEmpty());
  }

  void closingAsksAboutUnsavedConditionals() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    auto window = std::make_unique<ExperimentWindow>(bridge, true, settings());
    QVERIFY(window->load_queue(queue_file(sim)));
    auto* editor = window->open_conditionals_editor(QStringLiteral("system"));
    editor->add_disable(QStringLiteral("anything"));
    QVERIFY(editor->modified());
    int asked = 0;
    auto answer = pychron::ui::ConditionalsEditorWindow::Unsaved::Cancel;
    editor->set_ask_unsaved([&](const QString&) {
      ++asked;
      return answer;
    });
    window->show();
    QVERIFY(!window->close());
    QCOMPARE(asked, 1);
    QVERIFY(editor->modified());
    answer = pychron::ui::ConditionalsEditorWindow::Unsaved::Discard;
    QVERIFY(window->close());
    QCOMPARE(asked, 2);
    window.reset();
  }

  void theQueueConditionalsAreFixedWhileRunning() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    QVERIFY(window.queue_conditionals_combo()->isEnabled());
    bool combo_enabled = true, queue_set = true, row_set = false;
    connect(&bridge, &ExperimentBridge::runStarted, this, [&](const pychron::experiment::executor::RunStarted& e) {
      if (e.row != 0) return;
      window.model().set_frozen(1);
      combo_enabled = window.queue_conditionals_combo()->isEnabled();
      queue_set = window.model().set_queue_conditionals("default_unknown");
      // Rows the executor has not reached still take conditionals.
      row_set = window.model().set_conditionals({2}, {"default_unknown"});
    });
    window.executor()->request_start();
    QTRY_VERIFY_WITH_TIMEOUT(!window.executor()->running(), 60000);
    QVERIFY(!combo_enabled);
    QVERIFY(!queue_set);
    QVERIFY(row_set);
    QCOMPARE(window.model().queue().runs[2].conditionals.size(), std::size_t{1});
    QTRY_VERIFY(window.queue_conditionals_combo()->isEnabled());
  }

  // ---- a held queue -------------------------------------------------------------

  // Resume and Skip Held Run are enabled only while the queue holds; the
  // health bar says why and counts down; Resume ends the hold.
  void aHeldQueueIsShownAndResumeEndsTheHold() {
    pychron::ui::test::SimLab sim;
    pychron::SimTransport* down = hold_the_first_run(sim);
    QVERIFY(down != nullptr);
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    main.set_experiment(&bridge, true, queue_file(sim), [this] { return settings(); });
    main.experiment_action()->trigger();
    ExperimentWindow* window = main.experiment_window();
    QVERIFY(window != nullptr);
    window->set_confirm([](const QString&, const QString&) { return true; });
    auto* pane = window->executor();
    QVERIFY(!window->resume_held_action()->isEnabled());
    QVERIFY(!window->skip_held_action()->isEnabled());
    QVERIFY(!main.health_bar()->holding());

    window->start();
    QTRY_VERIFY_WITH_TIMEOUT(window->skip_held_action()->isEnabled(), 30000);
    QVERIFY(pane->holding());
    QVERIFY(window->resume_held_action()->isEnabled());  // the held row has not started: both
    QVERIFY(!window->model().status(0).has_value());
    QVERIFY(main.health_bar()->holding());
    const QString name = QString::fromStdString(sim.spec->links().front()->transport().name());
    QVERIFY2(main.health_bar()->hold_text().startsWith(QStringLiteral("holding: ")), qPrintable(main.health_bar()->hold_text()));
    // The link is named once in the reason.
    QCOMPARE(main.health_bar()->hold_text().count(name), 1);
    QVERIFY2(main.health_bar()->hold_text().contains(QStringLiteral(" · ends in ")), qPrintable(main.health_bar()->hold_text()));
    QVERIFY(contains(pane->events(), QStringLiteral("holding: ")));
    QVERIFY(contains(pane->events(), QStringLiteral("(%1) until ").arg(name)));

    // Back, and settling for an hour: Resume starts the held run now.
    down->set_down(false);
    QTRY_VERIFY_WITH_TIMEOUT(sim.spec->links().front()->transport().health().state != pychron::HealthState::Down, 30000);
    QVERIFY(pane->holding());
    window->resume_held_action()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->holding(), 30000);
    QVERIFY(!window->resume_held_action()->isEnabled());
    QVERIFY(!window->skip_held_action()->isEnabled());
    QVERIFY(!main.health_bar()->holding());
    QVERIFY(contains(pane->events(), QStringLiteral("resume requested")));
    QVERIFY(contains(pane->events(), QStringLiteral("resumed")));
    QTRY_VERIFY_WITH_TIMEOUT(window->model().status(0).has_value(), 30000);

    pane->request_abort();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 30000);
    QVERIFY(!window->resume_held_action()->isEnabled());
    main.set_experiment(nullptr, false);
  }

  // Skip Held Run drops the row held for; with the link still down, the
  // queue holds again for the next one. Resume while down holds again too.
  void skipDropsTheHeldRowAndTheQueueHoldsForTheNext() {
    pychron::ui::test::SimLab sim;
    QVERIFY(hold_the_first_run(sim) != nullptr);
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QVERIFY(window.load_queue(queue_file(sim)));
    int asked = 0;
    window.set_confirm([&](const QString&, const QString&) {
      ++asked;
      return true;
    });
    auto* pane = window.executor();
    pane->request_skip();  // nothing held: not asked
    QCOMPARE(asked, 0);
    window.start();
    QTRY_VERIFY_WITH_TIMEOUT(pane->skip_enabled(), 30000);
    pane->request_skip();  // the window is not shown: its menu's actions are not live, the pane's buttons are
    QCOMPARE(asked, 1);
    QVERIFY(contains(pane->events(), QStringLiteral("skip requested: row 0 is dropped")));
    QTRY_VERIFY_WITH_TIMEOUT(count(pane->events(), QStringLiteral("holding: ")) == 2, 30000);
    QVERIFY(!contains(pane->events(), QStringLiteral("resumed")));  // a skip is not a resume
    QTRY_VERIFY_WITH_TIMEOUT(pane->skip_enabled(), 30000);
    QVERIFY(!window.model().status(0).has_value());
    QVERIFY(!window.model().status(1).has_value());

    pane->request_resume();  // the link is still down: it holds again
    QTRY_VERIFY_WITH_TIMEOUT(count(pane->events(), QStringLiteral("holding: ")) == 3, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(pane->resume_enabled(), 30000);

    pane->request_abort();
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 30000);
    QVERIFY(!pane->resume_enabled());
    QVERIFY(!pane->skip_enabled());
  }

  // The countdown is to the hold limit, on the clock the bar is given (the
  // line's: CoreBridge::now), at two instants and past the limit.
  void theHoldCountsDownOnTheLinesClock() {
    using namespace std::chrono_literals;
    pychron::VirtualClock clock;
    const pychron::Clock::Participant self(clock, "test");
    pychron::ui::HealthBar bar(nullptr, [&clock] { return clock.now(); });
    QVERIFY(!bar.holding());
    QVERIFY(bar.hold_text().isEmpty());
    bar.show_hold(QStringLiteral("link down: spectrometer"), clock.now() + 2h);
    QCOMPARE(bar.hold_text(), QStringLiteral("holding: link down: spectrometer · ends in 2:00:00"));
    clock.sleep_for(90s);
    bar.refresh();
    QCOMPARE(bar.hold_text(), QStringLiteral("holding: link down: spectrometer · ends in 1:58:30"));
    QCOMPARE(bar.hold_text_at(clock.now() + 118min + 29s), QStringLiteral("holding: link down: spectrometer · ends in 0:00:01"));
    clock.sleep_for(3h);
    bar.refresh();
    QCOMPARE(bar.hold_text(), QStringLiteral("holding: link down: spectrometer · ends in 0:00:00"));
    bar.clear_hold();
    QVERIFY(!bar.holding());
    QVERIFY(bar.hold_text().isEmpty());
  }

  void mainWindowOffersTheExperimentWindow() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    QVERIFY(!main.experiment_action()->isEnabled());
    const fs::path queue = queue_file(sim);
    main.set_experiment(&bridge, true, queue, [this] { return settings(); });
    QVERIFY(main.experiment_action()->isEnabled());
    QVERIFY(main.experiment_window() == nullptr);
    main.experiment_action()->trigger();
    QVERIFY(main.experiment_window() != nullptr);
    QVERIFY(main.experiment_window()->isVisible());
    QCOMPARE(main.experiment_window()->model().rowCount(), 3);
    QVERIFY(main.experiment_window()->path() == queue);
    main.set_experiment(nullptr, false);
    QVERIFY(!main.experiment_action()->isEnabled());
    QVERIFY(main.experiment_window() == nullptr);
  }

  // The one File > Preferences… is in every window's bar, and opens over the
  // window in front.
  void experimentAndScriptEditorOfferPreferences() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    main.set_preferences_settings([this] { return settings(); });
    main.set_experiment(&bridge, true, std::nullopt, [this] { return settings(); });
    main.experiment_action()->trigger();
    ExperimentWindow* window = main.experiment_window();
    pychron::ui::ScriptEditorWindow* editor = window->open_script_editor();
    const auto file_menu = [](QMainWindow* w) {
      QMenuBar* bar = pychron::ui::MenuHub::instance().bar_for(w);
      return pychron::ui::MenuHub::instance().menus(bar).at(static_cast<int>(pychron::ui::MenuHub::Menu::File));
    };
    QAction* action = main.preferences_action();
    QVERIFY(file_menu(window)->actions().contains(action));
    QVERIFY(file_menu(editor)->actions().contains(action));
    QCOMPARE(action->menuRole(), QAction::PreferencesRole);
    QCOMPARE(action->shortcut(), QKeySequence(QKeySequence::Preferences));

    window->activateWindow();
    if (!QTest::qWaitForWindowActive(window)) QSKIP("this platform does not activate windows");
    action->trigger();  // over the experiment window
    auto dialogs = window->findChildren<pychron::ui::PreferencesDialog*>();
    QCOMPARE(dialogs.size(), 1);
    QVERIFY(dialogs.front()->isVisible());
    // From the script editor: the same dialog, raised, not doubled.
    editor->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(editor));
    action->trigger();
    QCOMPARE(main.findChildren<pychron::ui::PreferencesDialog*>().size(), 1);
    dialogs.front()->reject();
    QTRY_COMPARE(main.findChildren<pychron::ui::PreferencesDialog*>().size(), 0);  // deleted on close

    editor->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(editor));
    action->trigger();  // now over the editor
    QCOMPARE(editor->findChildren<pychron::ui::PreferencesDialog*>().size(), 1);
    main.set_experiment(nullptr, false);
  }

  // ---- where the queue dialogs start (queue-directory design, section 3) ------

  void openStartsInTheLabsQueuesWithNoLastQueue() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    QTemporaryDir dir;
    ExperimentWindow window(bridge, true,
                            std::make_unique<QSettings>(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    QString start;
    window.set_pick_open_file([&](const QString& s) {
      start = s;
      return QString();
    });
    window.open_dialog();
    QCOMPARE(start, QString::fromStdString((sim.dir / "queues").string()));
  }

  void openStartsWhereTheLastQueueWas() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    QTemporaryDir dir;
    ExperimentWindow window(bridge, true,
                            std::make_unique<QSettings>(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    fs::create_directories(sim.dir / "elsewhere");
    const fs::path q = sim.dir / "elsewhere" / "q.toml";
    QVERIFY(pychron::experiment::save_queue_file(q.string(), sim.queue()));
    QString error;
    QVERIFY2(window.load_queue(q, &error), qPrintable(error));
    QString start;
    window.set_pick_open_file([&](const QString& s) {
      start = s;
      return QString();
    });
    window.open_dialog();
    QCOMPARE(start, QString::fromStdString((sim.dir / "elsewhere").string()));
  }

  void openFallsBackWhenTheLastQueuesDirectoryIsGone() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    QTemporaryDir dir;
    ExperimentWindow window(bridge, true,
                            std::make_unique<QSettings>(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    fs::create_directories(sim.dir / "elsewhere");
    const fs::path q = sim.dir / "elsewhere" / "q.toml";
    QVERIFY(pychron::experiment::save_queue_file(q.string(), sim.queue()));
    QString error;
    QVERIFY2(window.load_queue(q, &error), qPrintable(error));
    fs::remove_all(sim.dir / "elsewhere");
    QString start;
    window.set_pick_open_file([&](const QString& s) {
      start = s;
      return QString();
    });
    window.open_dialog();
    QCOMPARE(start, QString::fromStdString((sim.dir / "queues").string()));
  }

  void savingANewQueueMakesQueues() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    QTemporaryDir dir;
    ExperimentWindow window(bridge, true,
                            std::make_unique<QSettings>(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    fs::remove_all(sim.dir / "queues");
    QString error;
    QVERIFY2(window.new_queue(&error), qPrintable(error));
    QString start;
    bool made = false;
    const fs::path out = sim.dir / "queues" / "new.toml";
    window.set_pick_save_file([&](const QString& s) {
      start = s;
      made = fs::is_directory(sim.dir / "queues");
      return QString::fromStdString(out.string());
    });
    window.save_as_dialog();
    QVERIFY(made);
    QCOMPARE(start, QString::fromStdString((sim.dir / "queues").string()));
    QVERIFY(window.path().has_value());
    QCOMPARE(*window.path(), out);
    QVERIFY(fs::exists(out));
  }

  void aQueueNamedAtStartIsFoundInTheLab() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    const fs::path in_lab = sim.dir / "queues" / "t041-start.toml";
    QVERIFY(pychron::experiment::save_queue_file(in_lab.string(), sim.queue()));
    pychron::ui::MainWindow main(*sim.line);
    main.set_experiment(&bridge, true, fs::path("t041-start.toml"), [this] { return settings(); });
    main.experiment_action()->trigger();
    QVERIFY(main.experiment_window() != nullptr);
    QVERIFY(main.experiment_window()->path().has_value());
    QCOMPARE(*main.experiment_window()->path(), in_lab);
    main.set_experiment(nullptr, false);
  }

  // Every window here was given its settings: none fell back to the application's.
  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(TestExperimentWindow)
#include "test_experiment_window.moc"
