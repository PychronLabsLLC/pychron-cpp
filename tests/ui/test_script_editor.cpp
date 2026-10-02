// The script editor against a scratch copy of the example lab: highlighting,
// completion, the static check and estimate, gosub navigation, saving.

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "code_editor.hpp"
#include "experiment_fixture.hpp"
#include "pychron/scripting/script_host.hpp"
#include "script_editor_window.hpp"
#include "script_highlighter.hpp"

using pychron::scripting::ScriptKind;
using pychron::ui::CodeEditor;
using pychron::ui::ScriptEditorWindow;
using pychron::ui::ScriptHighlighter;
namespace fs = std::filesystem;
namespace lab = pychron::experiment::lab;

namespace {

std::string read(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

class TestScriptEditor : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;
  fs::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  bool python_ = false;

  std::unique_ptr<QSettings> settings() const {
    return std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
  }

 private slots:
  void initTestCase() {
    dir_ = pychron::ui::test::scratch_lab();
    fs::create_directories(dir_ / "scripts" / "lib");
    std::ofstream(dir_ / "scripts" / "lib" / "pump.py") << "def main():\n    sleep(5)\n";
    std::ofstream(dir_ / "scripts" / "extraction" / "with_gosub.py")
        << "def main():\n    gosub('pump')\n    sleep(1)\n";
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
    python_ = pychron::scripting::make_script_host()->available();
  }
  void cleanupTestCase() {
    lab_.reset();
    fs::remove_all(dir_);
  }

  void highlighterKnowsTheVocabulary() {
    QTextDocument doc;
    ScriptHighlighter h(&doc, ScriptKind::Extraction);
    QCOMPARE(h.role_of(QStringLiteral("sleep")), std::optional(ScriptHighlighter::Role::Command));
    QCOMPARE(h.role_of(QStringLiteral("while")), std::optional(ScriptHighlighter::Role::Keyword));
    QCOMPARE(h.role_of(QStringLiteral("run_identifier")), std::optional(ScriptHighlighter::Role::Context));
    QCOMPARE(h.role_of(QStringLiteral("len")), std::optional(ScriptHighlighter::Role::Builtin));
    QVERIFY(!h.role_of(QStringLiteral("frobnicate")));
    QVERIFY(!h.role_of(QStringLiteral("signal_pump_time_start")));  // post-measurement only
    ScriptHighlighter pm(&doc, ScriptKind::PostMeasurement);
    QCOMPARE(pm.role_of(QStringLiteral("signal_pump_time_start")), std::optional(ScriptHighlighter::Role::Command));
  }

  void editorCompletesAndFindsGosubs() {
    CodeEditor editor;
    editor.set_completion_words({QStringLiteral("sleep"), QStringLiteral("set_motor"), QStringLiteral("close")});
    QCOMPARE(editor.completions_for(QStringLiteral("s")), (QStringList{QStringLiteral("sleep"), QStringLiteral("set_motor")}));
    QVERIFY(editor.completions_for(QStringLiteral("sleep")).isEmpty());
    editor.setPlainText(QStringLiteral("def main():\n    gosub('pump')\n"));
    QCOMPARE(editor.gosub_at(2, 13), std::optional<QString>(QStringLiteral("pump")));
    QVERIFY(!editor.gosub_at(2, 2));
    QVERIFY(!editor.gosub_at(9, 0));
    editor.set_diagnostics({{2, true, QStringLiteral("bad")}});
    QCOMPARE(editor.extraSelections().size(), 1);
    QCOMPARE(editor.extraSelections().front().cursor.selectedText(), QStringLiteral("gosub('pump')"));
    editor.go_to_line(2);
    QCOMPARE(editor.textCursor().blockNumber(), 1);
  }

  void opensChecksEditsAndSaves() {
    ScriptEditorWindow w(*lab_, settings());
    QVERIFY(w.script_names().contains(QStringLiteral("extraction/sim_extract")));
    QVERIFY(w.script_names().contains(QStringLiteral("post_measurement/sim_pump")));
    QVERIFY(w.script_names().contains(QStringLiteral("lib/pump")));
    QCOMPARE(w.document_count(), 0);
    QVERIFY(!w.open(ScriptKind::Extraction, QStringLiteral("no_such_script")));

    QVERIFY(w.open(ScriptKind::Extraction, QStringLiteral("sim_extract")));
    QCOMPARE(w.current_name(), QStringLiteral("extraction/sim_extract"));
    QVERIFY(!w.modified());
    QTest::qWait(50);  // the highlighter's delayed pass must not count as an edit
    QVERIFY(!w.modified());
    QVERIFY(w.current_editor()->completion_words().contains(QStringLiteral("sleep")));
    if (python_) {
      QVERIFY2(w.estimate_text().startsWith(QStringLiteral("Estimate ")), qPrintable(w.estimate_text()));
      QVERIFY(w.diagnostic_lines().isEmpty());
    } else {
      QVERIFY(w.estimate_text().startsWith(QStringLiteral("Not checked")));
    }
    // Opening it again switches to the tab instead of opening a second.
    QVERIFY(w.open(ScriptKind::Extraction, QStringLiteral("sim_extract")));
    QCOMPARE(w.document_count(), 1);

    // An unknown command is reported on its line, after the debounce.
    w.current_editor()->moveCursor(QTextCursor::End);
    w.current_editor()->insertPlainText(QStringLiteral("    frobnicate(3)\n"));  // as typing does
    QVERIFY(w.modified());
    if (python_) {
      QTRY_VERIFY_WITH_TIMEOUT(!w.diagnostic_lines().isEmpty(), 5000);
      QVERIFY(w.diagnostic_lines().front().contains(QStringLiteral("frobnicate")));
      QVERIFY(w.diagnostic_lines().front().startsWith(QStringLiteral("10: error")));
    // Listed in line order.
    w.current_editor()->moveCursor(QTextCursor::Start);
    w.current_editor()->insertPlainText(QStringLiteral("zap = nope\n"));
    w.check_now();
    QVERIFY(w.diagnostic_lines().size() >= 2);
    QVERIFY(w.diagnostic_lines().front().startsWith(QStringLiteral("1: ")));
    w.current_editor()->undo();
    w.check_now();
      QVERIFY(w.estimate_text().contains(QStringLiteral("error")));
      QCOMPARE(w.current_editor()->diagnostics().size(), std::size_t{1});
      QCOMPARE(w.current_editor()->diagnostics().front().line, 10);
    }
    QString error;
    QVERIFY2(w.save(&error), qPrintable(error));
    QVERIFY(!w.modified());
    QVERIFY(read(dir_ / "scripts" / "extraction" / "sim_extract.py").find("frobnicate(3)") != std::string::npos);
  }

  void newScriptsAndGosubs() {
    ScriptEditorWindow w(*lab_, settings());
    QSignalSpy changed(&w, &ScriptEditorWindow::scriptsChanged);
    QString error;
    QVERIFY2(w.new_script(ScriptKind::Extraction, QStringLiteral("co2:degas"), &error), qPrintable(error));
    QVERIFY(fs::is_regular_file(dir_ / "scripts" / "extraction" / "co2" / "degas.py"));
    QCOMPARE(w.current_name(), QStringLiteral("extraction/co2:degas"));
    QVERIFY(w.script_names().contains(QStringLiteral("extraction/co2:degas")));
    QCOMPARE(changed.count(), 1);
    if (python_) QVERIFY2(w.estimate_text().startsWith(QStringLiteral("Estimate ")), qPrintable(w.estimate_text()));
    QVERIFY(!w.new_script(ScriptKind::Extraction, QStringLiteral("co2:degas"), &error));
    QVERIFY(error.contains(QStringLiteral("exists")));
    QVERIFY(!w.new_script(ScriptKind::Extraction, QStringLiteral("../out"), &error));

    QVERIFY(w.open(ScriptKind::Extraction, QStringLiteral("with_gosub")));
    QCOMPARE(w.current_editor()->gosub_at(2, 12), std::optional<QString>(QStringLiteral("pump")));
    QVERIFY(w.follow_gosub(QStringLiteral("pump")));
    QCOMPARE(w.current_name(), QStringLiteral("extraction/lib:pump"));
    QVERIFY(w.open(ScriptKind::Extraction, QStringLiteral("with_gosub")));
    QVERIFY(!w.follow_gosub(QStringLiteral("nowhere")));
    QVERIFY(w.estimate_text().contains(QStringLiteral("does not resolve")));
    if (python_) {
      w.check_now();
      QVERIFY2(w.estimate_text().startsWith(QStringLiteral("Estimate 0:00:06")), qPrintable(w.estimate_text()));
    }
    fs::remove_all(dir_ / "scripts" / "extraction" / "co2");
  }

  void unsavedEditsAsk() {
    auto w = std::make_unique<ScriptEditorWindow>(*lab_, settings());
    QVERIFY(w->open(ScriptKind::PostMeasurement, QStringLiteral("sim_pump")));
    const std::string before = read(dir_ / "scripts" / "post_measurement" / "sim_pump.py");
    w->current_editor()->appendPlainText(QStringLiteral("# edited"));
    int asked = 0;
    auto answer = ScriptEditorWindow::Unsaved::Cancel;
    w->set_ask_unsaved([&](const QString& name) {
      ++asked;
      [&] { QCOMPARE(name, QStringLiteral("post_measurement/sim_pump")); }();
      return answer;
    });
    w->show();
    QVERIFY(!w->close_current());
    QVERIFY(!w->close());
    QCOMPARE(asked, 2);
    QCOMPARE(w->document_count(), 1);
    answer = ScriptEditorWindow::Unsaved::Discard;
    QVERIFY(w->close_current());
    QCOMPARE(w->document_count(), 0);
    QCOMPARE(read(dir_ / "scripts" / "post_measurement" / "sim_pump.py"), before);

    QVERIFY(w->open(ScriptKind::PostMeasurement, QStringLiteral("sim_pump")));
    w->current_editor()->appendPlainText(QStringLiteral("# saved on close"));
    answer = ScriptEditorWindow::Unsaved::Save;
    QVERIFY(w->close());
    QVERIFY(read(dir_ / "scripts" / "post_measurement" / "sim_pump.py").find("# saved on close") != std::string::npos);
    std::ofstream(dir_ / "scripts" / "post_measurement" / "sim_pump.py") << before;
  }
};

QTEST_MAIN(TestScriptEditor)
#include "test_script_editor.moc"
