// ExperimentWindow end to end on the sim lab (pumped 400x): open the example
// queue, run it, follow it; confirmations, saving, and Window > Experiment.

#include <filesystem>
#include <memory>

#include <QAction>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/queue_toml.hpp"

using pychron::experiment::run::RunState;
using pychron::ui::ExperimentBridge;
using pychron::ui::ExperimentWindow;
using pychron::ui::QueueTableModel;
namespace fs = std::filesystem;

class TestExperimentWindow : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;

  std::unique_ptr<QSettings> settings() const {
    return std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
  }
  // The example queue as the fixture runs it (scripts dropped without Python).
  fs::path queue_file(const pychron::ui::test::SimLab& sim, const char* name = "queue.toml") const {
    const fs::path p = sim.dir / name;
    if (!pychron::experiment::save_queue_file(p.string(), sim.queue())) qFatal("cannot write the queue");
    return p;
  }
  static bool contains(const QStringList& lines, const QString& text) {
    for (const auto& l : lines)
      if (l.contains(text)) return true;
    return false;
  }

 private slots:
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
    pane->request_start();
    QVERIFY(pane->running());
    QVERIFY(window.model().locked());
    QVERIFY(!pane->start_enabled());
    // The run factory adds nothing while the queue runs.
    auto form = window.factory()->form();
    form.identifier = "20001";
    window.factory()->set_form(form);
    QVERIFY(!window.factory()->add_enabled());
    QVERIFY(!window.factory()->add());
    QVERIFY(!window.load_queue(queue_file(sim, "other.toml"), &error));
    QCOMPARE(error, QStringLiteral("a queue is running"));

    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    for (int row = 0; row < 3; ++row) QCOMPARE(window.model().status(row), std::optional<RunState>(RunState::Success));
    QVERIFY(!window.model().locked());
    QCOMPARE(pane->progress_text(), QStringLiteral("3/3 run(s)"));
    QVERIFY(contains(pane->events(), QStringLiteral("queue completed")));
    QVERIFY(contains(pane->events(), QStringLiteral("run 1 66001")));
    QVERIFY(contains(pane->events(), QStringLiteral("peak center Ar40 on H1")));
    QCOMPARE(pane->state_text(), QStringLiteral("idle"));
    QVERIFY(pane->spool_text().isEmpty());
    QVERIFY(counts_max > 1);
    QCOMPARE(pane->counts_maximum(), 1);  // the last block (the peak center) has no counts
    // The last run's evolutions and peak center.
    auto* evo = window.evolutions();
    QVERIFY(evo->point_count(pychron::experiment::collect::SeriesKind::Signal) > 0);
    QVERIFY(evo->graph_count() > 0);
    QVERIFY(evo->title_text().startsWith(QStringLiteral("66001")));
    QCOMPARE(evo->peak_centers().size(), 1);
    QVERIFY(evo->peak_centers().front().contains(QStringLiteral("table updated")));
    evo->set_kind(pychron::experiment::collect::SeriesKind::Baseline);
    QVERIFY(evo->point_count(pychron::experiment::collect::SeriesKind::Baseline) > 0);
    QVERIFY(fs::exists(sim.dir / "data" / "records" / "66001" / "66001-2.json"));
    QVERIFY(pane->start_enabled());
    QVERIFY(window.factory()->add_enabled());  // unlocked again
  }

  void cancelAsksFirstAndStartsFromTheSelectedRow() {
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
    window.start();
    QTRY_VERIFY_WITH_TIMEOUT(window.model().status(1).has_value(), 30000);
    QVERIFY(!window.model().status(0).has_value());

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
    QCOMPARE(window.model().status(1), std::optional<RunState>(RunState::Cancelled));
    QVERIFY(!window.model().status(2).has_value());
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
};

QTEST_MAIN(TestExperimentWindow)
#include "test_experiment_window.moc"
