// The Rehearse Script window (spec
// docs/superpowers/specs/2026-10-20-script-rehearsal-design.md, section 6.2):
// a script stepped against a copy of the example line, painted on a
// read-only canvas, while the live line does not move.

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <QFormLayout>
#include <QLineEdit>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "canvas_view.hpp"
#include "code_editor.hpp"
#include "core_bridge.hpp"
#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "script_editor_window.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/scripting/script_host.hpp"
#include "rehearsal_window.hpp"
#include "settings_guard.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"
#include "ui_fixture.hpp"

using pychron::scripting::ScriptKind;
using pychron::ui::RehearsalWindow;
namespace fs = std::filesystem;
namespace lab = pychron::experiment::lab;

namespace {

std::string read(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// What the log's row says, all columns.
QString row_text(const QTableWidget& log, int row) {
  QStringList cells;
  for (int c = 0; c < log.columnCount(); ++c) {
    const QTableWidgetItem* item = log.item(row, c);
    cells << (item != nullptr ? item->text() : QString());
  }
  return cells.join(QLatin1Char('|'));
}

QString outcome(const QTableWidget& log, int row) {
  const QTableWidgetItem* item = log.item(row, 3);
  return item != nullptr ? item->text() : QString();
}

// Row `row` is there and waits (no outcome yet).
bool waits_at(const QTableWidget& log, int row) {
  return log.rowCount() == row + 1 && outcome(log, row).isEmpty() && log.item(row, 2) != nullptr;
}

}  // namespace

class TestRehearsalWindow : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  fs::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  std::unique_ptr<pychron::scripting::IScriptHost> host_;
  // The bridge after the line, which it reads.
  std::unique_ptr<pychron::systems::ExtractionLine> line_;
  std::unique_ptr<pychron::ui::CoreBridge> bridge_;
  QString sim_extract_;

  std::unique_ptr<QSettings> settings() const {
    return std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
  }
  std::unique_ptr<RehearsalWindow> make_window() {
    return std::make_unique<RehearsalWindow>(*host_, *bridge_, line_->switches().interlock_sets(), *lab_, settings());
  }

 private slots:
  void initTestCase() {
    host_ = pychron::scripting::make_script_host();
    if (!host_->available()) QSKIP("built without embedded Python");
    dir_ = pychron::ui::test::scratch_lab();
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
    sim_extract_ = QString::fromStdString(read(dir_ / "scripts" / "extraction" / "sim_extract.py"));
    line_ = pychron::ui::test::make_example_line();
    bridge_ = std::make_unique<pychron::ui::CoreBridge>(*line_);
    QVERIFY(line_->start());
    QTRY_VERIFY_WITH_TIMEOUT(bridge_->state().started, 5000);
  }
  void cleanupTestCase() {
    bridge_.reset();
    line_.reset();
    lab_.reset();
    if (!dir_.empty()) fs::remove_all(dir_);
    // Every window here was given its settings: none fell back to the application's.
    QCOMPARE(app_settings_.keys(), QStringList());
  }

  void StepsSimExtract() {
    auto w = make_window();
    const auto before = bridge_->state().valves;
    QVERIFY(before.contains("P2"));
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
    QCOMPARE(w->windowTitle(), QStringLiteral("Rehearse sim_extract.py"));
    QCOMPARE(w->status_text(), QStringLiteral("Rehearsal: no valve is actuated"));
    QVERIFY(w->canvas()->read_only());
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    QCOMPARE(w->log()->item(0, 2)->text(), QStringLiteral("close('C')"));
    QCOMPARE(w->source()->current_line(), 12);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 2), 5000);
    QCOMPARE(outcome(*w->log(), 1), QStringLiteral("would open"));
    QCOMPARE(w->log()->item(1, 1)->text(), QStringLiteral("sim_extract.py:14"));
    QCOMPARE(w->canvas()->valve("P2")->state(), pychron::ValveState::Open);
    QVERIFY(w->canvas()->valve("P2")->marked());  // changed by the last step
    QCOMPARE(bridge_->state().valves, before);    // the live line did not move
    // The script stands at the sleep after the open: that line is current.
    QCOMPARE(w->source()->current_line(), 15);
  }

  // The model would bank a second go-ahead sent while one step waits; the
  // window sends only one per stop.
  void StepTwiceAdvancesOneStep() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    QVERIFY(w->step_enabled());
    w->step();
    QVERIFY(!w->step_enabled());
    w->step();  // nothing waits: ignored
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);
    // A banked go-ahead would let row 2 through at once; give it the chance.
    QTest::qWait(300);
    QVERIFY2(waits_at(*w->log(), 1), qPrintable(row_text(*w->log(), w->log()->rowCount() - 1)));
    QVERIFY(w->step_enabled());
  }

  void RunToEndFinishes() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    w->run_to_end();
    QTRY_VERIFY_WITH_TIMEOUT(w->log()->rowCount() > 0 && outcome(*w->log(), w->log()->rowCount() - 1) ==
                                                              QStringLiteral("finished"),
                             5000);
    QVERIFY(!w->running());
    QVERIFY(!w->step_enabled());
    // The canvas keeps the final rehearsal state: sim_extract closes P1 last.
    QCOMPARE(w->canvas()->valve("P1")->state(), pychron::ValveState::Closed);
  }

  void RehearsesUnsavedText() {
    auto w = make_window();
    QString text = sim_extract_;
    text.replace(QStringLiteral("    close('C')\n"), QStringLiteral("    close('A')\n    close('C')\n"));
    QVERIFY(text != sim_extract_);
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), text);
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    QCOMPARE(w->log()->item(0, 2)->text(), QStringLiteral("close('A')"));
  }

  void CheckFailureShowsDiagnostics() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("bad.py"), QStringLiteral("def main():\n    open('Z')\n"));
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(w->log()->rowCount() == 1, 5000);
    QVERIFY2(row_text(*w->log(), 0).contains(QLatin1Char('Z')), qPrintable(row_text(*w->log(), 0)));
    QCOMPARE(w->log()->item(0, 3)->foreground().color(), pychron::ui::theme().error_text);
    QVERIFY(!w->running());
    QElapsedTimer t;
    t.start();
    w->stop();
    QVERIFY(t.elapsed() < 1000);
    QCOMPARE(w->log()->rowCount(), 1);
  }

  void RestartWhileBlockedStartsOver() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 2), 5000);
    QElapsedTimer t;
    t.start();
    w->restart();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    QVERIFY(t.elapsed() < 5000);
    QCOMPARE(w->log()->item(0, 2)->text(), QStringLiteral("close('C')"));
    // A fresh copy of the live line: P2 is as the line has it again.
    QCOMPARE(w->canvas()->valve("P2")->state(), bridge_->state().valves.at("P2"));
  }

  void PendingValueEntry() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("read.py"),
                  QStringLiteral("def main():\n    p = get_pressure('x', 'nogauge')\n    sleep(1)\n"));
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(w->log()->rowCount() == 1 && w->log()->cellWidget(0, 3) != nullptr, 5000);
    auto* entry = qobject_cast<QLineEdit*>(w->log()->cellWidget(0, 3));
    QVERIFY(entry != nullptr);
    QVERIFY(!w->step_enabled());  // a read waits for its value, not a go-ahead
    QTest::keyClicks(entry, QStringLiteral("1e-9"));
    QTest::keyClick(entry, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);
    QCOMPARE(outcome(*w->log(), 0), QStringLiteral("= 1e-09"));
    QVERIFY(w->log()->cellWidget(0, 3) == nullptr);
    QCOMPARE(w->log()->item(1, 2)->text(), QStringLiteral("sleep(1)"));
  }

  // Spec 6.2: a second rehearsal asks before it replaces a running one.
  void ReplaceRunningAsks() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    w->set_confirm_replace([] { return false; });
    w->set_script(ScriptKind::Extraction, QStringLiteral("other.py"), QStringLiteral("def main():\n    sleep(1)\n"));
    QCOMPARE(w->windowTitle(), QStringLiteral("Rehearse sim_extract.py"));
    QVERIFY(w->running());
    QVERIFY(waits_at(*w->log(), 0));
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);  // still the first one, still stepping

    w->set_confirm_replace([] { return true; });
    w->set_script(ScriptKind::Extraction, QStringLiteral("other.py"), QStringLiteral("def main():\n    sleep(1)\n"));
    QCOMPARE(w->windowTitle(), QStringLiteral("Rehearse other.py"));
    QVERIFY(!w->running());
    QCOMPARE(w->log()->rowCount(), 0);
    QVERIFY(w->source()->toPlainText().contains(QStringLiteral("sleep(1)")));
  }

  void FieldsReachTheScript() {
    auto w = make_window();
    pychron::scripting::ValueMap defaults{{"analysis_type", std::string("blank_unknown")}};
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_, defaults);
    QVERIFY(w->fields()->rowCount() >= 5);
    auto* type = w->findChild<QLineEdit*>(QStringLiteral("field_analysis_type"));
    QVERIFY(type != nullptr);
    QCOMPARE(type->text(), QStringLiteral("blank_unknown"));
    w->start();
    // A blank gets no pipette: after close('C') comes sleep(duration).
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);
    QVERIFY2(w->log()->item(1, 2)->text().startsWith(QStringLiteral("sleep(")), qPrintable(row_text(*w->log(), 1)));
    // Restart keeps what the form holds.
    w->restart();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    QCOMPARE(type->text(), QStringLiteral("blank_unknown"));
  }

  void GeometrySaved() {
    {
      auto w = make_window();
      w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
      w->show();
      QVERIFY(QTest::qWaitForWindowExposed(w.get()));
      w->start();
      QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
      w->close();  // stops the rehearsal too
      QVERIFY(!w->running());
    }
    QSettings s(tmp_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QVERIFY(!s.value(QStringLiteral("rehearsal_window/geometry")).toByteArray().isEmpty());
  }

  // A step's change is in view: even in a small window the canvas is fitted
  // whole, the valve the step changed with it.
  void ChangedValveIsBroughtIntoView() {
    auto w = make_window();
    w->set_script(ScriptKind::Extraction, QStringLiteral("sim_extract.py"), sim_extract_);
    w->resize(500, 360);
    w->show();
    QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    w->canvas()->resize(160, 120);
    const auto in_view = [&](const char* valve) {
      const QRectF view = w->canvas()->mapToScene(w->canvas()->viewport()->rect()).boundingRect();
      return view.intersects(w->canvas()->valve(valve)->sceneBoundingRect());
    };
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 1), 5000);
    w->step();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 2), 5000);
    QVERIFY(w->canvas()->valve("P2")->marked());
    QVERIFY(in_view("P2"));
    w->stop();
  }

  void ShortcutCatalogHasNoClash() {
    using pychron::ui::Shortcut;
    using pychron::ui::ShortcutContext;
    QCOMPARE(pychron::ui::key(Shortcut::RehearseStep), QKeySequence(Qt::Key_F10));
    QCOMPARE(pychron::ui::key(Shortcut::RehearseRunToEnd), QKeySequence(Qt::SHIFT | Qt::Key_F10));
    const auto& c = pychron::ui::shortcut_catalog();
    for (std::size_t i = 0; i < c.size(); ++i)
      for (std::size_t j = i + 1; j < c.size(); ++j)
        if (!c[i].key.isEmpty() && c[i].key == c[j].key)
          QVERIFY2(!pychron::ui::overlap(c[i].context, c[j].context), qPrintable(c[i].command + QStringLiteral(" / ") + c[j].command));
    QVERIFY(pychron::ui::overlap(ShortcutContext::RehearsalWindow, ShortcutContext::Everywhere));
    QVERIFY(!pychron::ui::overlap(ShortcutContext::RehearsalWindow, ShortcutContext::FileMenu));
    QVERIFY(!pychron::ui::overlap(ShortcutContext::RehearsalWindow, ShortcutContext::ScriptEditor));
    // The window's toolbar carries them.
    auto w = make_window();
    QCOMPARE(w->step_action()->shortcut(), pychron::ui::key(Shortcut::RehearseStep));
    QCOMPARE(w->run_to_end_action()->shortcut(), pychron::ui::key(Shortcut::RehearseRunToEnd));
  }

  // Spec 7: a rehearsal actuates nothing. Three of the example line's scripts
  // run to their end; no switch moved, none counted a command, and the line's
  // bus saw no ValveChanged or ActuationFailed.
  void NothingIsActuated() {
    const auto before = line_->switches().list();
    QVERIFY(!before.empty());
    std::atomic<int> changed{0};
    std::atomic<int> failed{0};
    auto changed_sub = line_->bus().subscribe<pychron::ValveChanged>(
        [&](const pychron::ValveChanged&) { ++changed; });
    auto failed_sub = line_->bus().subscribe<pychron::ActuationFailed>(
        [&](const pychron::ActuationFailed&) { ++failed; });

    struct Case {
      ScriptKind kind;
      const char* name;
      fs::path path;
    };
    const std::array<Case, 3> cases{{
        {ScriptKind::Extraction, "sim_extract.py", dir_ / "scripts" / "extraction" / "sim_extract.py"},
        {ScriptKind::Extraction, "sim_air.py", dir_ / "scripts" / "extraction" / "sim_air.py"},
        {ScriptKind::PostMeasurement, "sim_pump.py", dir_ / "scripts" / "post_measurement" / "sim_pump.py"},
    }};
    for (const Case& c : cases) {
      auto w = make_window();
      w->set_script(c.kind, QString::fromUtf8(c.name), QString::fromStdString(read(c.path)));
      w->start();
      QTRY_VERIFY_WITH_TIMEOUT(w->log()->rowCount() > 0, 5000);
      bool done = false;
      for (int i = 0; i < 400 && !done; ++i) {
        QTest::qWait(25);
        const int last = w->log()->rowCount() - 1;
        if (last < 0) continue;
        if (outcome(*w->log(), last) == QStringLiteral("finished") || !w->running()) {
          done = true;
        } else if (auto* entry = qobject_cast<QLineEdit*>(w->log()->cellWidget(last, 3)); entry != nullptr) {
          QTest::keyClicks(entry, QStringLiteral("1"));  // a value read waits for one
          QTest::keyClick(entry, Qt::Key_Return);
        } else if (w->step_enabled()) {
          w->run_to_end();
        }
      }
      QVERIFY2(done, c.name);
      const int last = w->log()->rowCount() - 1;
      QVERIFY2(!w->running(), c.name);
      // Not vacuous: the script did try to open or close valves.
      int moves = 0;
      for (int r = 0; r <= last; ++r) {
        const QString o = outcome(*w->log(), r);
        if (o.startsWith(QStringLiteral("would open")) || o.startsWith(QStringLiteral("would close"))) ++moves;
      }
      QVERIFY2(moves >= 1, c.name);
      QVERIFY2(last >= 0 && outcome(*w->log(), last) == QStringLiteral("finished"),
               qPrintable(QString::fromUtf8(c.name) + QLatin1Char(' ') + row_text(*w->log(), last)));
    }

    const auto after = line_->switches().list();
    QCOMPARE(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
      QCOMPARE(QString::fromStdString(after[i].name), QString::fromStdString(before[i].name));
      QVERIFY2(after[i].state == before[i].state, after[i].name.c_str());
      QVERIFY2(after[i].stats == before[i].stats, after[i].name.c_str());
    }
    QCOMPARE(changed.load(), 0);
    QCOMPARE(failed.load(), 0);
  }

  // The main window owns one rehearsal window, made on first use, over the
  // session's lab; a second rehearsal brings the same window forward.
  void MainWindowOwnsOne() {
    pychron::ui::test::SimLab sim;
    pychron::ui::ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    QVERIFY(!main.rehearse_action()->isEnabled());  // no session yet (App scope: its own state)
    QVERIFY(main.rehearse(ScriptKind::Extraction, QStringLiteral("sim_extract"), sim_extract_) == nullptr);
    main.set_experiment(&bridge, true, {}, [this] { return settings(); });
    QVERIFY(main.rehearse_action()->isEnabled());
    RehearsalWindow* w = main.rehearse(ScriptKind::Extraction, QStringLiteral("sim_extract"), sim_extract_);
    QVERIFY(w != nullptr);
    QCOMPARE(main.rehearsal_window(), w);
    QCOMPARE(w->windowTitle(), QStringLiteral("Rehearse sim_extract"));
    QVERIFY(w->isVisible());
    w->start();
    QTRY_VERIFY_WITH_TIMEOUT(waits_at(*w->log(), 0), 5000);
    w->set_confirm_replace([] { return true; });
    QCOMPARE(main.rehearse(ScriptKind::Extraction, QStringLiteral("sim_blank"), sim_extract_), w);
    QCOMPARE(w->windowTitle(), QStringLiteral("Rehearse sim_blank"));
    // The editor's Rehearse… reaches it through the experiment window.
    main.experiment_action()->trigger();
    QVERIFY(main.experiment_window() != nullptr);
    pychron::ui::ScriptEditorWindow* editor = main.experiment_window()->open_script_editor();
    QVERIFY(editor->open(ScriptKind::Extraction, QStringLiteral("sim_extract")));
    QVERIFY(editor->rehearse_current());
    QCOMPARE(w->windowTitle(), QStringLiteral("Rehearse sim_extract"));
    editor->close();
    main.experiment_window()->close();
  }
};

QTEST_MAIN(TestRehearsalWindow)
#include "test_rehearsal_window.moc"
