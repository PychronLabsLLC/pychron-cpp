// The conditionals editor: the table model over one file's conditionals, the
// form for one conditional, and the window against a scratch copy of the
// example lab.

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

#include "conditional_form.hpp"
#include "conditional_table_model.hpp"
#include "experiment_fixture.hpp"

using pychron::experiment::ActionSpec;
using pychron::experiment::Conditional;
using pychron::experiment::ConditionalKind;
using pychron::experiment::ConditionalSet;
using pychron::ui::ConditionalForm;
using pychron::ui::ConditionalTableModel;
namespace fs = std::filesystem;
namespace ex = pychron::experiment;

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

const QStringList kTypes{QStringLiteral("unknown"), QStringLiteral("air"), QStringLiteral("cocktail"),
                         QStringLiteral("blank")};

}  // namespace

class TestConditionalsEditor : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() { qRegisterMetaType<Conditional>(); }

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
};

QTEST_MAIN(TestConditionalsEditor)
#include "test_conditionals_editor.moc"
