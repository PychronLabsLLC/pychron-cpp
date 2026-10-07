// ExperimentWindow end to end on the sim lab (simulated time paced at 400x): open the example
// queue, run it, follow it; confirmations, saving, and View > Experiment.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QMenu>
#include <QMenuBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "conditionals_editor_window.hpp"
#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "preferences_dialog.hpp"
#include "script_editor_window.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/queue_toml.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"

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
    // The beam measures what the line's source holds, and the example's
    // extraction script lets a pipette of the tank's air into the line for
    // each run that is not a blank (sim_extract.py).
    QVERIFY(sim.line->sim() != nullptr);
    pane->request_start();
    QVERIFY(pane->running());
    QVERIFY(window.model().live());  // editable after the rows the executor reached
    QVERIFY(!pane->start_enabled());
    QVERIFY(!window.load_queue(queue_file(sim, "other.toml"), &error));
    QCOMPARE(error, QStringLiteral("a queue is running"));

    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    for (int row = 0; row < 3; ++row) QCOMPARE(window.model().status(row), std::optional<RunState>(RunState::Success));
    QVERIFY(!window.model().live());
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
    // The live fits: a curve per signal series and its intercept listed.
    QVERIFY(evo->fit_curve_count() > 0);
    QVERIFY(!evo->intercept_lines().isEmpty());
    const auto ar40 = evo->fit_of({"Ar40", "H1", pychron::experiment::collect::SeriesKind::Signal});
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
    evo->set_kind(pychron::experiment::collect::SeriesKind::Baseline);
    QVERIFY(evo->point_count(pychron::experiment::collect::SeriesKind::Baseline) > 0);
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
    QVERIFY(pane->start_enabled());
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
  void aRunsLogShowsInTheExecutorPane() {
    if (!pychron::scripting::make_script_host()->available()) QSKIP("needs embedded Python to run laser_extract.py");
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    ExperimentWindow window(bridge, true, settings());
    QString error;
    QVERIFY2(window.load_queue(sim.dir / "experiment.laser.toml", &error), qPrintable(error));
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
    window.table()->selectionModel()->select(window.model().index(1, 0),
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
    window.table()->selectAll();
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
    window.table()->selectAll();
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
};

QTEST_MAIN(TestExperimentWindow)
#include "test_experiment_window.moc"
