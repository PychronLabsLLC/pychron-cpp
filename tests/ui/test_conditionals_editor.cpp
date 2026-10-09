// The conditionals editor: the table model over one file's conditionals, the
// form for one conditional, and the window against a scratch copy of the
// example lab.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include "conditional_form.hpp"
#include "conditional_table_model.hpp"
#include "conditionals_editor_window.hpp"
#include "experiment_fixture.hpp"
#include "settings_guard.hpp"

using pychron::experiment::ActionSpec;
using pychron::experiment::Conditional;
using pychron::experiment::ConditionalKind;
using pychron::experiment::ConditionalSet;
using pychron::ui::ConditionalForm;
using pychron::ui::ConditionalTableModel;
using pychron::ui::ConditionalsEditorWindow;
namespace fs = std::filesystem;
namespace ex = pychron::experiment;
namespace lab = pychron::experiment::lab;

Q_DECLARE_METATYPE(pychron::experiment::Conditional)

namespace {

Conditional cond(ConditionalKind kind, const std::string& check, const std::string& name = {}) {
  Conditional c;
  c.kind = kind;
  c.check = check;
  c.name = name;
  c.action.type = ex::fields_of(kind).default_action;
  return c;
}

ConditionalSet set_of(std::vector<Conditional> items) {
  ConditionalSet s;
  s.items = std::move(items);
  return s;
}

QString cell(const ConditionalTableModel& m, int row, int col) { return m.data(m.index(row, col)).toString(); }

// Every authored field, for comparing what the form hands back.
bool same(const Conditional& a, const Conditional& b) {
  return a.name == b.name && a.kind == b.kind && a.check == b.check && a.start == b.start &&
         a.frequency == b.frequency && a.ntrips == b.ntrips && a.window == b.window && a.mapper == b.mapper &&
         a.analysis_types == b.analysis_types && a.abbreviated_count_ratio == b.abbreviated_count_ratio &&
         a.action == b.action && a.resume == b.resume && a.truncate == b.truncate && a.terminate == b.terminate;
}

template <class W>
W* child(QWidget& parent, const char* name) {
  auto* w = parent.findChild<W*>(QString::fromLatin1(name));
  if (!w) qFatal("no widget named %s", name);
  return w;
}

void type_into(QLineEdit* edit, const QString& text) {
  edit->setFocus();
  edit->selectAll();
  QTest::keyClicks(edit, text);
}

void choose(QComboBox* combo, const QString& text) {
  const int i = combo->findText(text);
  if (i < 0) qFatal("no item %s", qPrintable(text));
  combo->setCurrentIndex(i);
  emit combo->activated(i);
}

std::string read(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const QStringList kTypes{QStringLiteral("unknown"), QStringLiteral("air"), QStringLiteral("cocktail"),
                         QStringLiteral("blank")};

}  // namespace

class TestConditionalsEditor : public QObject {
  Q_OBJECT

  // The window tests: a fresh scratch lab per test; the lab outlives the window.
  QTemporaryDir tmp_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  fs::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  int settings_n_ = 0;

  fs::path file(const std::string& name) const { return dir_ / "conditionals" / (name + ".toml"); }
  QString settings_path() const { return tmp_.filePath(QStringLiteral("settings%1.ini").arg(settings_n_)); }
  std::unique_ptr<ConditionalsEditorWindow> window() {
    auto w = std::make_unique<ConditionalsEditorWindow>(
        *lab_, std::make_unique<QSettings>(settings_path(), QSettings::IniFormat));
    w->set_ask_unsaved([](const QString&) { return ConditionalsEditorWindow::Unsaved::Discard; });
    w->set_confirm([](const QString&) { return true; });
    return w;
  }
  // Adds a truncation with `check` through the form's widgets; returns its row.
  static int add_truncation(ConditionalsEditorWindow& w, const QString& check) {
    const int row = w.add_conditional(ConditionalKind::Truncation);
    type_into(child<QLineEdit>(*w.form(), "check"), check);
    return w.current_row() >= 0 ? w.current_row() : row;
  }

 private slots:
  void initTestCase() { qRegisterMetaType<Conditional>(); }
  void init() {
    ++settings_n_;
    dir_ = pychron::ui::test::scratch_lab();
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
  }
  void cleanup() {
    lab_.reset();
    std::error_code ec;
    fs::permissions(dir_ / "conditionals", fs::perms::owner_all, ec);
    fs::remove_all(dir_, ec);
  }

  // ---- ConditionalTableModel ----

  void modelSortsByFileOrder() {
    ConditionalTableModel m;
    m.set(set_of({cond(ConditionalKind::PostRun, "Ar40 < 1"), cond(ConditionalKind::Truncation, "Ar40 > 1"),
                  cond(ConditionalKind::Action, "Ar40 > 2")}));
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(cell(m, 0, ConditionalTableModel::Kind), QStringLiteral("truncation"));
    QCOMPARE(cell(m, 1, ConditionalTableModel::Kind), QStringLiteral("action"));
    QCOMPARE(cell(m, 2, ConditionalTableModel::Kind), QStringLiteral("post_run"));
    QCOMPARE(cell(m, 0, ConditionalTableModel::Check), QStringLiteral("Ar40 > 1"));
    QCOMPARE(cell(m, 0, ConditionalTableModel::Name), QStringLiteral("truncation:Ar40 > 1"));
    QCOMPARE(cell(m, 0, ConditionalTableModel::Action), QStringLiteral("truncate"));
    QCOMPARE(cell(m, 2, ConditionalTableModel::Action), QStringLiteral("cancel"));
    QVERIFY(!m.modified());
  }

  void modelAddRemoveDuplicateMove() {
    ConditionalTableModel m;
    m.set(set_of({cond(ConditionalKind::Truncation, "Ar40 > 1", "a"), cond(ConditionalKind::PostRun, "Ar40 < 1")}));
    QSignalSpy changed(&m, &ConditionalTableModel::changed);
    QCOMPARE(m.add(ConditionalKind::Truncation), 1);  // after the last truncation
    QCOMPARE(changed.count(), 1);
    QVERIFY(m.modified());
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(m.add(ConditionalKind::Termination), 2);  // between truncations and post_run
    QCOMPARE(m.add(ConditionalKind::PostRun), 4);
    QVERIFY(m.remove(4));
    QVERIFY(m.remove(2));
    QVERIFY(!m.remove(7));
    QCOMPARE(m.rowCount(), 3);

    QCOMPARE(m.duplicate(0), 1);
    QCOMPARE(cell(m, 1, ConditionalTableModel::Name), QStringLiteral("a_copy"));
    QCOMPARE(m.rowCount(), 4);
    changed.clear();
    QVERIFY(m.move_down(0));  // a <-> a_copy
    QCOMPARE(changed.count(), 1);
    QCOMPARE(cell(m, 0, ConditionalTableModel::Name), QStringLiteral("a_copy"));
    QVERIFY(m.move_up(1));
    QCOMPARE(cell(m, 0, ConditionalTableModel::Name), QStringLiteral("a"));
    QVERIFY(!m.move_up(0));
    QVERIFY(!m.move_down(2));  // the last truncation does not cross into post_run
    QVERIFY(!m.move_up(3));
    m.mark_clean();
    QVERIFY(!m.modified());
  }

  void modelReplaceResortsOnKindChange() {
    ConditionalTableModel m;
    m.set(set_of({cond(ConditionalKind::Truncation, "Ar40 > 1", "a"), cond(ConditionalKind::Truncation, "Ar40 > 2", "b"),
                  cond(ConditionalKind::PostRun, "Ar40 < 1", "p")}));
    auto c = m.conditionals().items[0];
    c.kind = ConditionalKind::PostRun;
    c.action.type = ActionSpec::Type::Cancel;
    QCOMPARE(m.replace(0, c), 2);
    QCOMPARE(cell(m, 0, ConditionalTableModel::Name), QStringLiteral("b"));
    QCOMPARE(cell(m, 2, ConditionalTableModel::Name), QStringLiteral("a"));
    QCOMPARE(m.row_of(QStringLiteral("a")), 2);
    QCOMPARE(m.row_of(QStringLiteral("zz")), -1);
    QCOMPARE(m.replace(9, c), -1);
  }

  void modelErrorsPerRow() {
    ConditionalTableModel m;
    m.set(set_of({cond(ConditionalKind::Truncation, "Ar40 > 1")}));
    QVERIFY(!m.has_errors());
    auto c = m.conditionals().items[0];
    c.check = "Ar40 >";
    QCOMPARE(m.replace(0, c), 0);
    QVERIFY(!m.error(0).isEmpty());
    QVERIFY(m.has_errors());
    QCOMPARE(m.data(m.index(0, 0), Qt::ToolTipRole).toString(), m.error(0));
    c.check = "Ar40 > 3";
    m.replace(0, c);
    QVERIFY(m.error(0).isEmpty());
    QVERIFY(!m.has_errors());
    const int row = m.add(ConditionalKind::Action);  // no check yet
    QCOMPARE(m.error(row), QStringLiteral("missing 'check'"));
  }

  void modelDuplicateNamesAreErrors() {
    ConditionalTableModel m;
    m.set(set_of({cond(ConditionalKind::Truncation, "Ar40 > 1"), cond(ConditionalKind::Truncation, "Ar40 > 1")}));
    QVERIFY(m.error(0).isEmpty());
    const QString name = QString::fromStdString(ex::default_name(ConditionalKind::Truncation, "Ar40 > 1"));
    QCOMPARE(m.error(1), QStringLiteral("duplicate name '%1'").arg(name));
    QVERIFY(m.has_errors());
    auto c = m.conditionals().items[1];
    c.name = "second";
    m.replace(1, c);
    QVERIFY(!m.has_errors());
  }

  void modelDefaultNameFollowsTheCheck() {
    // A parsed file carries the default name; it must not stick when the check changes.
    auto parsed = ex::parse_conditionals("[[truncations]]\ncheck = \"Ar40 > 1\"\n");
    QVERIFY(parsed.has_value());
    ConditionalTableModel m;
    m.set(*parsed);
    QVERIFY(m.conditionals().items[0].name.empty());
    auto c = m.conditionals().items[0];
    c.check = "Ar40 > 2";
    m.replace(0, c);
    QCOMPARE(cell(m, 0, ConditionalTableModel::Name), QStringLiteral("truncation:Ar40 > 2"));
  }

  void modelModifiedComparesContent() {
    ConditionalTableModel m;
    m.set(set_of({cond(ConditionalKind::Truncation, "Ar40 > 1")}));
    auto original = m.conditionals().items[0];
    auto c = original;
    c.start = 20;
    m.replace(0, c);
    QVERIFY(m.modified());
    m.replace(0, original);
    QVERIFY(!m.modified());
  }

  void modelDisableList() {
    ConditionalTableModel m;
    QSignalSpy changed(&m, &ConditionalTableModel::changed);
    m.set_disable({QStringLiteral("system:gauge_high")});
    QCOMPARE(changed.count(), 1);
    QCOMPARE(m.disable(), QStringList{QStringLiteral("system:gauge_high")});
    QCOMPARE(m.conditionals().disable, std::vector<std::string>{"system:gauge_high"});
    QVERIFY(m.modified());
    m.set_disable({});
    QVERIFY(!m.modified());
  }

  // ---- ConditionalForm ----

  void formKindShowsFields() {
    ConditionalForm f(kTypes);
    f.set_conditional(cond(ConditionalKind::Truncation, "Ar40 > 1"));
    QVERIFY(f.shows_gating() && f.shows_ratio() && f.shows_action() && !f.shows_resume() && !f.shows_run_flags());
    QCOMPARE(child<QComboBox>(f, "action")->count(), 1);
    QVERIFY(!child<QCheckBox>(f, "action_quick")->isHidden());
    f.set_conditional(cond(ConditionalKind::Termination, "Ar40 > 1"));
    QVERIFY(f.shows_gating() && !f.shows_ratio() && !f.shows_action());
    f.set_conditional(cond(ConditionalKind::Action, "Ar40 > 1"));
    QVERIFY(f.shows_action() && f.shows_resume() && !f.shows_ratio());
    f.set_conditional(cond(ConditionalKind::Modification, "Ar40 > 1"));
    QVERIFY(f.shows_run_flags() && f.shows_ratio() && f.shows_action());
    QCOMPARE(child<QComboBox>(f, "action")->count(), 7);
    f.set_conditional(cond(ConditionalKind::PreRun, "Ar40 > 1"));
    QVERIFY(!f.shows_gating() && !f.shows_ratio() && !f.shows_resume());
  }

  void formRoundTripsEveryField() {
    ConditionalForm f(kTypes);
    QSignalSpy edited(&f, &ConditionalForm::edited);
    for (ConditionalKind kind : ex::kFileOrder) {
      const auto& fields = ex::fields_of(kind);
      Conditional c = cond(kind, "Ar40 > 1", "named");
      c.ntrips = 3;
      c.window = 10;
      c.mapper = "x + 1000";
      c.analysis_types = {"air", "blank"};
      if (fields.gating) {
        c.start = 20;
        c.frequency = 5;
      }
      if (fields.ratio) c.abbreviated_count_ratio = 1.0 / 3.0;
      if (fields.resume) c.resume = true;
      if (fields.run_flags) c.terminate = true;
      if (kind == ConditionalKind::Action) c.action = *ex::parse_action("set_param X=1.5");
      if (kind == ConditionalKind::Modification) c.action = *ex::parse_action("set_extract 10%,20%");
      if (kind == ConditionalKind::PostRun) c.action = *ex::parse_action("skip_n 3");
      f.set_conditional(c);
      QVERIFY2(same(f.conditional(), c), std::string(to_string(kind)).c_str());
    }
    QCOMPARE(edited.count(), 0);  // loading is not editing
    QCOMPARE(child<QLineEdit>(f, "action_steps")->isHidden(), true);
    QCOMPARE(child<QSpinBox>(f, "action_count")->value(), 3);
  }

  void formEditEmits() {
    ConditionalForm f(kTypes);
    f.set_conditional(cond(ConditionalKind::Truncation, "Ar40 > 1"));
    QSignalSpy edited(&f, &ConditionalForm::edited);
    type_into(child<QLineEdit>(f, "check"), QStringLiteral("Ar40 > 8e5"));
    QVERIFY(edited.count() > 0);
    QCOMPARE(edited.last().at(0).value<Conditional>().check, std::string("Ar40 > 8e5"));
    QCOMPARE(f.conditional().check, std::string("Ar40 > 8e5"));
    child<QSpinBox>(f, "start")->setValue(20);
    QCOMPARE(f.conditional().start, 20);
    child<QSpinBox>(f, "window")->setValue(10);
    QCOMPARE(f.conditional().window, std::optional<int>(10));
    child<QSpinBox>(f, "window")->setValue(0);
    QCOMPARE(f.conditional().window, std::optional<int>());
    child<QDoubleSpinBox>(f, "ratio")->setValue(0.5);
    QCOMPARE(f.conditional().abbreviated_count_ratio, 0.5);
    type_into(child<QLineEdit>(f, "name"), QStringLiteral("big"));
    QCOMPARE(f.conditional().name, std::string("big"));
    auto* types = child<QListWidget>(f, "types");
    types->item(0)->setCheckState(Qt::Checked);
    types->item(3)->setCheckState(Qt::Checked);
    QCOMPARE(f.conditional().analysis_types, (std::vector<std::string>{"unknown", "blank"}));
  }

  void formBadCheckShowsError() {
    ConditionalForm f(kTypes);
    f.set_conditional(cond(ConditionalKind::Truncation, "Ar40 > 1"));
    QVERIFY(f.check_error().isEmpty());
    type_into(child<QLineEdit>(f, "check"), QStringLiteral("Ar40 >"));
    QVERIFY(!f.check_error().isEmpty());
    type_into(child<QLineEdit>(f, "check"), QStringLiteral("Ar40 > 1"));
    QVERIFY(f.check_error().isEmpty());
    f.set_conditional(cond(ConditionalKind::Truncation, "Ar40 >"));
    QVERIFY(!f.check_error().isEmpty());
  }

  void formKindChangeResets() {
    ConditionalForm f(kTypes);
    Conditional c = cond(ConditionalKind::Action, "Ar40 > 1", "warn");
    c.action = *ex::parse_action("run_hook warn_operator");
    c.resume = true;
    c.start = 20;
    c.ntrips = 2;
    f.set_conditional(c);
    choose(child<QComboBox>(f, "kind"), QStringLiteral("truncation"));
    Conditional t = f.conditional();
    QCOMPARE(t.kind, ConditionalKind::Truncation);
    QVERIFY(!t.resume);
    QCOMPARE(t.action, ActionSpec{.type = ActionSpec::Type::Truncate});
    QCOMPARE(t.name, std::string("warn"));
    QCOMPARE(t.check, std::string("Ar40 > 1"));
    QCOMPARE(t.start, 20);
    QCOMPARE(t.ntrips, 2);
    child<QDoubleSpinBox>(f, "ratio")->setValue(0.5);
    choose(child<QComboBox>(f, "kind"), QStringLiteral("pre_run"));
    t = f.conditional();
    QCOMPARE(t.start, 0);
    QCOMPARE(t.frequency, 1);
    QCOMPARE(t.abbreviated_count_ratio, 1.0);
    QCOMPARE(t.action.type, ActionSpec::Type::Cancel);
    QVERIFY(ex::finalize(t).has_value());
  }

  void formActionParameters() {
    ConditionalForm f(kTypes);
    f.set_conditional(cond(ConditionalKind::Action, "Ar40 > 1"));
    choose(child<QComboBox>(f, "action"), QStringLiteral("set_param"));
    type_into(child<QLineEdit>(f, "action_name"), QStringLiteral("X"));
    type_into(child<QLineEdit>(f, "action_value"), QStringLiteral("1.5"));
    ActionSpec want{.type = ActionSpec::Type::SetParam, .name = "X", .value = 1.5};
    QCOMPARE(f.conditional().action, want);

    choose(child<QComboBox>(f, "action"), QStringLiteral("truncate"));
    child<QCheckBox>(f, "action_quick")->setChecked(true);
    QCOMPARE(f.conditional().action, (ActionSpec{.type = ActionSpec::Type::Truncate, .quick = true}));

    f.set_conditional(cond(ConditionalKind::Modification, "Ar40 > 1"));
    choose(child<QComboBox>(f, "action"), QStringLiteral("skip_n"));
    child<QSpinBox>(f, "action_count")->setValue(3);
    QCOMPARE(f.conditional().action, (ActionSpec{.type = ActionSpec::Type::SkipN, .count = 3}));

    choose(child<QComboBox>(f, "action"), QStringLiteral("set_extract"));
    type_into(child<QLineEdit>(f, "action_steps"), QStringLiteral("10%,20%"));
    ActionSpec steps{.type = ActionSpec::Type::SetExtract, .steps = {10, 20}, .percent = true};
    QCOMPARE(f.conditional().action, steps);
    QVERIFY(child<QCheckBox>(f, "action_percent")->isChecked());
    // Typed a key at a time: "1, 2" was the last text that is a list, and it stays.
    type_into(child<QLineEdit>(f, "action_steps"), QStringLiteral("1, 2,"));
    QCOMPARE(f.conditional().action, (ActionSpec{.type = ActionSpec::Type::SetExtract, .steps = {1, 2}, .percent = true}));
    QVERIFY(!f.action_error().isEmpty());
    type_into(child<QLineEdit>(f, "action_steps"), QStringLiteral("1,2"));
    child<QCheckBox>(f, "action_percent")->setChecked(false);
    QCOMPARE(f.conditional().action, (ActionSpec{.type = ActionSpec::Type::SetExtract, .steps = {1, 2}}));
    QVERIFY(f.action_error().isEmpty());

    choose(child<QComboBox>(f, "run_flag"), QStringLiteral("truncate"));
    QVERIFY(f.conditional().truncate && !f.conditional().terminate);
    choose(child<QComboBox>(f, "run_flag"), QStringLiteral("terminate"));
    QVERIFY(!f.conditional().truncate && f.conditional().terminate);
  }

  void formUnknownAnalysisTypeKept() {
    ConditionalForm f(kTypes);
    Conditional c = cond(ConditionalKind::Truncation, "Ar40 > 1");
    c.analysis_types = {"weird", "air"};
    f.set_conditional(c);
    QCOMPARE(f.conditional().analysis_types, (std::vector<std::string>{"weird", "air"}));
    child<QSpinBox>(f, "start")->setValue(4);  // an unrelated edit keeps it
    QCOMPARE(f.conditional().analysis_types, (std::vector<std::string>{"weird", "air"}));
  }

  // ---- ConditionalsEditorWindow ----

  void windowListsAndOpensSystemFirst() {
    auto w = window();
    QCOMPARE(w->file_names(), (QStringList{QStringLiteral("system"), QStringLiteral("default_unknown")}));
    QCOMPARE(w->current_name(), QStringLiteral("system"));
    QVERIFY(w->editable());
    QCOMPARE(w->model().rowCount(), 4);
    QVERIFY(!w->modified());
    QVERIFY(w->current_row() >= 0);  // a row is selected, so the form shows something
  }

  void windowAddEditSave() {
    auto w = window();
    QSignalSpy saved(w.get(), &ConditionalsEditorWindow::saved);
    add_truncation(*w, QStringLiteral("Ar40 > 8e5"));
    child<QSpinBox>(*w->form(), "start")->setValue(20);
    QVERIFY(w->modified());
    QVERIFY(w->windowTitle().contains(QLatin1Char('*')));
    QString error;
    QVERIFY2(w->save(&error), qPrintable(error));
    QVERIFY(!w->modified());
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.at(0).at(0).toString(), QStringLiteral("system"));

    // Spec section 1: the file is one the tools accept.
    auto parsed = ex::parse_conditionals(read(file("system")));
    QVERIFY2(parsed.has_value(), parsed ? "" : parsed.error().what.c_str());
    QCOMPARE(parsed->items.size(), std::size_t{5});
    const auto it = std::find_if(parsed->items.begin(), parsed->items.end(),
                                 [](const Conditional& c) { return c.check == "Ar40 > 8e5"; });
    QVERIFY(it != parsed->items.end());
    QCOMPARE(it->kind, ConditionalKind::Truncation);
    QCOMPARE(it->start, 20);
    for (const auto& d : ex::validate_conditionals(*parsed, lab_->metric_catalog())) QVERIFY2(!d.error, d.message.c_str());
    QVERIFY(w->status_text().contains(QStringLiteral("next queue start")));
  }

  void windowBadCheckBlocksSave() {
    auto w = window();
    const std::string before = read(file("system"));
    add_truncation(*w, QStringLiteral("Ar40 >"));
    QString error;
    QVERIFY(!w->save(&error));
    QVERIFY2(error.contains(QStringLiteral("Fix the errors")), qPrintable(error));
    QVERIFY(w->modified());
    QCOMPARE(read(file("system")), before);
    QVERIFY(!w->diagnostic_lines().isEmpty());  // listed at once, no delay
    QVERIFY(w->diagnostic_lines().first().startsWith(QStringLiteral("error: ")));
  }

  void windowCatalogErrorDoesNotBlock() {
    auto w = window();
    w->check_now();
    QVERIFY2(w->diagnostic_lines().isEmpty(), qPrintable(w->diagnostic_lines().join(QLatin1Char('|'))));
    add_truncation(*w, QStringLiteral("Xx99 > 1"));
    w->check_now();
    const QStringList lines = w->diagnostic_lines();
    QCOMPARE(lines.size(), 1);
    QVERIFY2(lines.first().startsWith(QStringLiteral("error: truncation:Xx99 > 1: ")), qPrintable(lines.first()));
    QVERIFY(w->save());
  }

  void windowDiagnosticsAfterDelay() {
    auto w = window();
    add_truncation(*w, QStringLiteral("Xx99 > 1"));
    QVERIFY(w->diagnostic_lines().isEmpty());  // not yet
    QTRY_COMPARE_WITH_TIMEOUT(w->diagnostic_lines().size(), 1, ConditionalsEditorWindow::kCheckDelayMs + 1500);
  }

  void windowDiagnosticSelectsRow() {
    auto w = window();
    const int row = add_truncation(*w, QStringLiteral("Xx99 > 1"));
    w->select_row(0);
    QVERIFY(w->current_row() != row);
    w->check_now();
    auto* list = child<QListWidget>(*w, "diagnostics");
    QCOMPARE(list->count(), 1);
    emit list->itemActivated(list->item(0));
    QCOMPARE(w->current_row(), row);
    QCOMPARE(w->form()->conditional().check, std::string("Xx99 > 1"));
  }

  void windowSelectionFollowsKindChange() {
    auto w = window();
    const int row = add_truncation(*w, QStringLiteral("Ar40 > 8e5"));
    choose(child<QComboBox>(*w->form(), "kind"), QStringLiteral("post_run"));
    QVERIFY(w->current_row() != row);
    QCOMPARE(w->model().conditionals().items[static_cast<std::size_t>(w->current_row())].check,
             std::string("Ar40 > 8e5"));
    QCOMPARE(w->form()->conditional().kind, ConditionalKind::PostRun);
  }

  void windowUnsavedPromptOnSwitchAndClose() {
    auto w = window();
    const std::string before = read(file("system"));
    add_truncation(*w, QStringLiteral("Ar40 > 8e5"));
    QStringList asked;
    auto answer = ConditionalsEditorWindow::Unsaved::Cancel;
    w->set_ask_unsaved([&](const QString& name) {
      asked.append(name);
      return answer;
    });
    w->show();
    QVERIFY(!w->open(QStringLiteral("default_unknown")));
    QVERIFY(!w->close());
    QCOMPARE(asked, (QStringList{QStringLiteral("system"), QStringLiteral("system")}));
    QCOMPARE(w->current_name(), QStringLiteral("system"));
    QVERIFY(w->modified());
    QCOMPARE(read(file("system")), before);

    answer = ConditionalsEditorWindow::Unsaved::Discard;
    QVERIFY(w->open(QStringLiteral("default_unknown")));
    QCOMPARE(w->current_name(), QStringLiteral("default_unknown"));
    QVERIFY(!w->modified());
    QCOMPARE(read(file("system")), before);

    add_truncation(*w, QStringLiteral("Ar40 > 9e5"));
    answer = ConditionalsEditorWindow::Unsaved::Save;
    QVERIFY(w->open(QStringLiteral("system")));
    QVERIFY(read(file("default_unknown")).find("Ar40 > 9e5") != std::string::npos);

    // Save chosen, but the file cannot be saved: stay.
    add_truncation(*w, QStringLiteral("Ar40 >"));
    QVERIFY(!w->open(QStringLiteral("default_unknown")));
    QCOMPARE(w->current_name(), QStringLiteral("system"));
    answer = ConditionalsEditorWindow::Unsaved::Discard;
    QVERIFY(w->close());
  }

  void windowNewAndDelete() {
    auto w = window();
    QSignalSpy files_changed(w.get(), &ConditionalsEditorWindow::filesChanged);
    QString error;
    QVERIFY(!w->new_file(QStringLiteral("a/b"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!w->new_file(QStringLiteral("system"), &error));
    QVERIFY2(error.contains(QStringLiteral("exists")), qPrintable(error));
    QVERIFY(w->new_file(QStringLiteral("run_x"), &error));
    QCOMPARE(w->current_name(), QStringLiteral("run_x"));
    QVERIFY(w->file_names().contains(QStringLiteral("run_x")));
    QVERIFY(!fs::exists(file("run_x")));  // in memory until saved
    QCOMPARE(w->model().rowCount(), 0);
    add_truncation(*w, QStringLiteral("Ar40 > 8e5"));
    QVERIFY(w->save(&error));
    QVERIFY(fs::exists(file("run_x")));
    QCOMPARE(files_changed.count(), 1);
    QVERIFY(w->status_text().contains(QStringLiteral("next run")));

    bool referenced = true;
    QString asked;
    bool yes = false;
    w->set_referenced([&](const QString&) { return referenced; });
    w->set_confirm([&](const QString& q) {
      asked = q;
      return yes;
    });
    QVERIFY(!w->delete_file(QStringLiteral("run_x")));
    QVERIFY(fs::exists(file("run_x")));
    QVERIFY2(asked.contains(QStringLiteral("queue")), qPrintable(asked));
    yes = true;
    QVERIFY(w->delete_file(QStringLiteral("run_x")));
    QVERIFY(!fs::exists(file("run_x")));
    QVERIFY(!w->file_names().contains(QStringLiteral("run_x")));
    QCOMPARE(w->current_name(), QStringLiteral("system"));
    QCOMPARE(files_changed.count(), 2);

    // A new file that is discarded was never a file.
    QVERIFY(w->new_file(QStringLiteral("scratch")));
    add_truncation(*w, QStringLiteral("Ar40 > 1"));
    w->set_ask_unsaved([](const QString&) { return ConditionalsEditorWindow::Unsaved::Discard; });
    QVERIFY(w->open(QStringLiteral("system")));
    QVERIFY(!w->file_names().contains(QStringLiteral("scratch")));
  }

  void windowCommentWarningOnce() {
    std::ofstream(file("noted")) << "# note\n[[truncations]]\ncheck = \"Ar40 > 1\"  # why\n";
    std::ofstream(file("hash_in_string")) << "[[truncations]]\nname = \"a#b\"\ncheck = \"Ar40 > 1\"\n";
    auto w = window();
    int asked = 0;
    bool yes = false;
    w->set_confirm([&](const QString& q) {
      ++asked;
      [&] { QVERIFY2(q.contains(QStringLiteral("comments")), qPrintable(q)); }();
      return yes;
    });
    QVERIFY(w->open(QStringLiteral("noted")));
    child<QSpinBox>(*w->form(), "start")->setValue(3);
    const std::string before = read(file("noted"));
    QVERIFY(!w->save());
    QCOMPARE(asked, 1);
    QCOMPARE(read(file("noted")), before);
    QVERIFY(w->modified());
    yes = true;
    QVERIFY(w->save());
    QCOMPARE(asked, 2);
    QVERIFY(read(file("noted")).find('#') == std::string::npos);
    child<QSpinBox>(*w->form(), "start")->setValue(4);
    QVERIFY(w->save());
    QCOMPARE(asked, 2);  // the comments are gone; nothing to ask

    QVERIFY(w->open(QStringLiteral("hash_in_string")));
    child<QSpinBox>(*w->form(), "start")->setValue(4);
    QVERIFY(w->save());
    QCOMPARE(asked, 2);  // a '#' inside a string is not a comment
  }

  void windowUnparsableFileNotEditable() {
    std::ofstream(file("broken")) << "[[truncations]]\ncheck = 3\n";
    auto w = window();
    QVERIFY(w->file_names().contains(QStringLiteral("broken")));
    QVERIFY(w->open(QStringLiteral("broken")));
    QCOMPARE(w->current_name(), QStringLiteral("broken"));
    QVERIFY(!w->editable());
    QVERIFY(!w->load_error().isEmpty());
    QVERIFY(!w->form()->isEnabled());
    QCOMPARE(w->add_conditional(ConditionalKind::Truncation), -1);
    QString error;
    QVERIFY(!w->save(&error));
    QCOMPARE(read(file("broken")), std::string("[[truncations]]\ncheck = 3\n"));
    QVERIFY(w->open(QStringLiteral("system")));
    QVERIFY(w->editable());
    QVERIFY(w->form()->isEnabled());
  }

  void windowWriteFailureKeepsModified() {
#ifdef Q_OS_WIN
    QSKIP("directory permissions do not block writes on Windows");
#else
    if (::geteuid() == 0) QSKIP("root ignores directory permissions");
    auto w = window();
    add_truncation(*w, QStringLiteral("Ar40 > 8e5"));
    const std::string before = read(file("system"));
    fs::permissions(dir_ / "conditionals", fs::perms::owner_read | fs::perms::owner_exec);
    QString error;
    const bool ok = w->save(&error);
    fs::permissions(dir_ / "conditionals", fs::perms::owner_all);
    QVERIFY(!ok);
    QVERIFY(!error.isEmpty());
    QVERIFY(w->modified());
    QCOMPARE(read(file("system")), before);
    QVERIFY(w->save(&error));  // and it works once the directory is writable again
#endif
  }

  void windowActionWithoutItsParametersBlocksSave() {
    auto w = window();
    const std::string before = read(file("system"));
    w->add_conditional(ConditionalKind::Action);
    type_into(child<QLineEdit>(*w->form(), "check"), QStringLiteral("Ar40 > 1"));
    for (const char* action : {"set_param", "run_hook"}) {
      choose(child<QComboBox>(*w->form(), "action"), QString::fromLatin1(action));
      QVERIFY2(!w->model().error(w->current_row()).isEmpty(), action);
      QVERIFY2(!w->save(), action);
      QCOMPARE(read(file("system")), before);
    }
    type_into(child<QLineEdit>(*w->form(), "action_name"), QStringLiteral("warn"));
    QVERIFY(w->model().error(w->current_row()).isEmpty());
    QVERIFY(w->save());
    QVERIFY(ex::parse_conditionals(read(file("system"))).has_value());

    w->add_conditional(ConditionalKind::Modification);
    type_into(child<QLineEdit>(*w->form(), "check"), QStringLiteral("Ar40 > 2"));
    choose(child<QComboBox>(*w->form(), "action"), QStringLiteral("set_extract"));
    QVERIFY(!w->save());
    type_into(child<QLineEdit>(*w->form(), "action_steps"), QStringLiteral("1,2"));
    QVERIFY(w->save());
    QVERIFY(ex::parse_conditionals(read(file("system"))).has_value());
  }

  void windowNewFileNeverLandsOnAnExistingOne() {
    if (!fs::exists(file("SYSTEM"))) QSKIP("the filesystem tells names apart by case");
    auto w = window();
    const std::string before = read(file("system"));
    QString error;
    QVERIFY(!w->new_file(QStringLiteral("System"), &error));
    QVERIFY2(error.contains(QStringLiteral("exists")), qPrintable(error));
    QCOMPARE(w->current_name(), QStringLiteral("system"));
    QCOMPARE(read(file("system")), before);
  }

  void windowDisableListSaved() {
    auto w = window();
    QVERIFY(w->open(QStringLiteral("default_unknown")));
    w->add_disable(QStringLiteral("system:gauge_high"));
    QVERIFY(w->modified());
    QVERIFY(w->save());
    QVERIFY(read(file("default_unknown")).starts_with("disable = [\"system:gauge_high\"]\n"));
    QCOMPARE(child<QListWidget>(*w, "disable")->count(), 1);
  }

  void windowRemembersLastFile() {
    {
      auto w = window();
      QVERIFY(w->open(QStringLiteral("default_unknown")));
      w->show();
      QVERIFY(w->close());
    }
    auto w = window();  // same settings file
    QCOMPARE(w->current_name(), QStringLiteral("default_unknown"));
  }

  void windowEmptyLab() {
    fs::remove_all(dir_ / "conditionals");
    auto w = window();
    QVERIFY(w->file_names().isEmpty());
    QVERIFY(w->current_name().isEmpty());
    QVERIFY(!w->editable());
    QVERIFY(!w->save());
    QVERIFY(w->new_file(QStringLiteral("system")));
    add_truncation(*w, QStringLiteral("Ar40 > 8e5"));
    QVERIFY(w->save());  // creates the directory
    QVERIFY(fs::exists(file("system")));
  }
  // Every window here was given its settings: none fell back to the application's.
  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(TestConditionalsEditor)
#include "test_conditionals_editor.moc"
