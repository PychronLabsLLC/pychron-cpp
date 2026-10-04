// QueueTableModel against the example lab (no hardware: only the lab files).

#include <filesystem>

#include <QBrush>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "queue_table_model.hpp"
#include "theme.hpp"

using pychron::experiment::QueueSpec;
using pychron::experiment::run::RunState;
using pychron::ui::QueueTableModel;
namespace exec = pychron::experiment::executor;
namespace lab = pychron::experiment::lab;

class TestQueueTableModel : public QObject {
  Q_OBJECT

  std::filesystem::path dir_;
  std::unique_ptr<lab::Lab> lab_;

  std::unique_ptr<QueueTableModel> model() {
    auto m = std::make_unique<QueueTableModel>(lab_->ids, [this](const QueueSpec& q) { return lab::check_lab_queue(*lab_, q); });
    m->set_queue(pychron::ui::test::example_queue(dir_, lab_->ids));
    return m;
  }
  static QString cell(const QueueTableModel& m, int row, int col) { return m.data(m.index(row, col)).toString(); }
  static std::vector<QString> identifiers(const QueueTableModel& m) {
    std::vector<QString> out;
    for (int r = 0; r < m.rowCount(); ++r) out.push_back(cell(m, r, QueueTableModel::Identifier));
    return out;
  }

 private slots:
  void initTestCase() {
    dir_ = pychron::ui::test::scratch_lab();
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
    QVERIFY(lab_->problems.empty());
  }
  void cleanupTestCase() {
    lab_.reset();
    std::filesystem::remove_all(dir_);
  }

  void rowsAndColumnsFromTheQueue() {
    auto m = model();
    QCOMPARE(m->rowCount(), 3);
    QCOMPARE(m->columnCount(), int(QueueTableModel::Count));
    QCOMPARE(m->headerData(QueueTableModel::Plan, Qt::Horizontal).toString(), QStringLiteral("Plan"));
    QCOMPARE(cell(*m, 0, QueueTableModel::Identifier), QStringLiteral("bu"));
    QCOMPARE(cell(*m, 0, QueueTableModel::Type), QStringLiteral("blank_unknown"));
    QCOMPARE(cell(*m, 1, QueueTableModel::Type), QStringLiteral("unknown"));
    QCOMPARE(cell(*m, 1, QueueTableModel::Extract), QStringLiteral("5 watts"));
    QCOMPARE(cell(*m, 1, QueueTableModel::Plan), QStringLiteral("sim_multicollect"));
    QVERIFY(cell(*m, 1, QueueTableModel::Estimate).contains(QLatin1Char(':')));
    QVERIFY(cell(*m, 1, QueueTableModel::Status).isEmpty());
    QVERIFY(m->runnable());
    QVERIFY(m->queue_diagnostics().isEmpty());
    QVERIFY(!m->data(m->index(1, QueueTableModel::Plan), Qt::BackgroundRole).isValid());
  }

  void editingRevalidatesAndFlagsTheRow() {
    auto m = model();
    QSignalSpy edited(m.get(), &QueueTableModel::edited);
    const QModelIndex plan = m->index(2, QueueTableModel::Plan);
    QVERIFY(m->flags(plan) & Qt::ItemIsEditable);
    QVERIFY(m->setData(plan, QStringLiteral("no_such_plan")));
    QCOMPARE(edited.count(), 1);
    QVERIFY(m->row_has_error(2));
    QVERIFY(!m->runnable());
    QVERIFY(m->data(plan, Qt::ToolTipRole).toString().contains(QStringLiteral("no_such_plan")));
    QCOMPARE(m->data(m->index(2, QueueTableModel::Identifier), Qt::BackgroundRole).value<QBrush>().color(),
             pychron::ui::theme().error_bg);
    QVERIFY(!m->row_has_error(1));
    QVERIFY(m->setData(plan, QStringLiteral("sim_multicollect")));
    QVERIFY(!m->row_has_error(2));
    QVERIFY(m->runnable());

    // Identifier edits re-derive the type; Type itself is read-only.
    QVERIFY(m->setData(m->index(1, QueueTableModel::Identifier), QStringLiteral("ba")));
    QCOMPARE(cell(*m, 1, QueueTableModel::Type), QStringLiteral("blank_air"));
    QVERIFY(!(m->flags(m->index(1, QueueTableModel::Type)) & Qt::ItemIsEditable));
    QVERIFY(!m->setData(m->index(1, QueueTableModel::Type), QStringLiteral("unknown")));

    // Text that does not parse is rejected and the cell keeps its value.
    QVERIFY(!m->setData(m->index(1, QueueTableModel::Extract), QStringLiteral("lots")));
    QVERIFY(!m->setData(m->index(1, QueueTableModel::Extract), QStringLiteral("5 parsecs")));
    QCOMPARE(cell(*m, 1, QueueTableModel::Extract), QStringLiteral("5 watts"));
    QVERIFY(m->setData(m->index(1, QueueTableModel::Extract), QStringLiteral("12.5%")));
    QCOMPARE(m->queue().runs[1].extraction.value, 12.5);
    QVERIFY(!m->setData(m->index(1, QueueTableModel::Position), QStringLiteral("x;y")));
    QVERIFY(m->setData(m->index(1, QueueTableModel::Position), QStringLiteral("4")));
    QCOMPARE(cell(*m, 1, QueueTableModel::Position), QStringLiteral("4"));
  }

  void rowOperations() {
    auto m = model();
    std::vector<std::size_t> moved;
    QVERIFY(!m->move_up({0}));
    QVERIFY(m->move_up({2}, &moved));
    QCOMPARE(moved, std::vector<std::size_t>{1});
    QCOMPARE(m->queue().runs[1].extraction.value, 8.0);
    QVERIFY(m->move_down({0}, &moved));
    QCOMPARE(moved, std::vector<std::size_t>{1});
    QCOMPARE(identifiers(*m), (std::vector<QString>{QStringLiteral("66001"), QStringLiteral("bu"), QStringLiteral("66001")}));
    QVERIFY(!m->move_down({2}));

    QVERIFY(m->duplicate({1}));
    QCOMPARE(m->rowCount(), 4);
    QCOMPARE(cell(*m, 2, QueueTableModel::Identifier), QStringLiteral("bu"));
    QVERIFY(m->remove({2, 3}));
    QCOMPARE(m->rowCount(), 2);

    QVERIFY(m->toggle_skip({0}));
    QVERIFY(m->queue().runs[0].skip);
    QVERIFY(cell(*m, 0, QueueTableModel::Estimate).isEmpty());
    QVERIFY(m->data(m->index(0, QueueTableModel::Identifier), Qt::ForegroundRole).isValid());
    QVERIFY(m->toggle_end_after(1));
    QVERIFY(m->queue().runs[1].end_after);
    QVERIFY(m->data(m->index(1, 0), Qt::ToolTipRole).toString().contains(QStringLiteral("ends after")));
  }

  void lockedRefusesEverything() {
    auto m = model();
    m->set_locked(true);
    QVERIFY(!(m->flags(m->index(1, QueueTableModel::Plan)) & Qt::ItemIsEditable));
    QVERIFY(!m->setData(m->index(1, QueueTableModel::Plan), QStringLiteral("x")));
    QVERIFY(!m->move_up({1}));
    QVERIFY(!m->duplicate({1}));
    QVERIFY(!m->remove({1}));
    QVERIFY(!m->toggle_skip({1}));
    QVERIFY(!m->toggle_end_after(1));
    QCOMPARE(m->rowCount(), 3);
    m->set_locked(false);
    QVERIFY(m->toggle_skip({1}));
  }

  // While a queue runs: rows the executor reached are read-only, and every
  // change goes through the committer first.
  void liveEditsGoThroughTheCommitter() {
    auto m = model();
    std::vector<std::pair<std::uint64_t, std::size_t>> offered;  // (base, rows)
    bool accept = true;
    m->set_live(0, [&](std::uint64_t base, const QueueSpec& q) -> pychron::Result<std::uint64_t> {
      offered.emplace_back(base, q.runs.size());
      if (!accept) return pychron::fail(pychron::ErrorKind::Config, "the queue changed");
      return base + 1;
    });
    QVERIFY(m->live());
    QSignalSpy refused(m.get(), &QueueTableModel::editRefused);
    QSignalSpy frozen(m.get(), &QueueTableModel::frozenChanged);
    m->set_frozen(1);
    QCOMPARE(frozen.count(), 1);
    m->set_frozen(0);  // never shrinks
    QCOMPARE(m->frozen_rows(), 1u);
    QVERIFY(!m->row_editable(0));
    QVERIFY(m->row_editable(1));
    QCOMPARE(m->insert_position(0), 1u);
    QVERIFY(!(m->flags(m->index(0, QueueTableModel::Plan)) & Qt::ItemIsEditable));
    QVERIFY(m->flags(m->index(1, QueueTableModel::Plan)) & Qt::ItemIsEditable);

    // Rows the executor reached: refused here, never offered.
    QVERIFY(!m->setData(m->index(0, QueueTableModel::Comment), QStringLiteral("x")));
    QVERIFY(!m->remove({0}));
    QVERIFY(!m->move_up({1}));
    QCOMPARE(refused.count(), 2);  // setData on a read-only cell does not get that far
    QVERIFY(offered.empty());

    // Accepted: kept, and the version moves on.
    QVERIFY(m->setData(m->index(2, QueueTableModel::Comment), QStringLiteral("late")));
    QVERIFY(m->remove({1}));
    QCOMPARE(offered, (std::vector<std::pair<std::uint64_t, std::size_t>>{{0, 3}, {1, 2}}));
    QCOMPARE(m->version(), 2u);
    QCOMPARE(m->rowCount(), 2);
    QCOMPARE(m->queue().runs[1].comment, std::string("late"));

    // Refused by the executor: nothing changes.
    accept = false;
    QVERIFY(!m->toggle_skip({1}));
    QCOMPARE(refused.count(), 3);
    QCOMPARE(refused.last().front().toString(), QStringLiteral("the queue changed"));
    QVERIFY(!m->queue().runs[1].skip);
    QCOMPARE(m->version(), 2u);

    // The executor's events: the echo of an accepted edit is ignored, a newer
    // change (a conditional's) is adopted with its frontier.
    exec::QueueEdited echo;
    echo.queue = m->queue();
    echo.queue.runs[1].comment = "stale";
    echo.version = 2;
    QVERIFY(!m->on_queue_edited(echo));
    QCOMPARE(m->queue().runs[1].comment, std::string("late"));
    exec::QueueEdited newer = echo;
    newer.version = 3;
    newer.frozen = 2;
    QVERIFY(m->on_queue_edited(newer));
    QCOMPARE(m->queue().runs[1].comment, std::string("stale"));
    QCOMPARE(m->version(), 3u);
    QCOMPARE(m->frozen_rows(), 2u);

    m->end_live();
    QVERIFY(!m->live());
    QVERIFY(m->row_editable(0));
    QVERIFY(m->toggle_skip({0}));
    QCOMPARE(offered.size(), 3u);  // not offered once the queue ended
  }

  void conditionalsColumn() {
    auto m = model();
    QCOMPARE(m->headerData(QueueTableModel::Conditionals, Qt::Horizontal).toString(), QStringLiteral("Conditionals"));
    QCOMPARE(m->headerData(QueueTableModel::Comment, Qt::Horizontal).toString(), QStringLiteral("Comment"));
    QCOMPARE(m->headerData(QueueTableModel::Estimate, Qt::Horizontal).toString(), QStringLiteral("Est."));
    QCOMPARE(cell(*m, 1, QueueTableModel::Conditionals), QString());
    QueueSpec q = m->queue();
    q.runs[1].conditionals = {{"system", "action"}, {"default_unknown", "truncate"}};
    m->set_queue(q);
    QCOMPARE(cell(*m, 1, QueueTableModel::Conditionals), QStringLiteral("system, default_unknown"));
    QVERIFY(!(m->flags(m->index(1, QueueTableModel::Conditionals)) & Qt::ItemIsEditable));
    QVERIFY(!m->setData(m->index(1, QueueTableModel::Conditionals), QStringLiteral("x")));
  }

  void setConditionals() {
    auto m = model();
    QSignalSpy edited(m.get(), &QueueTableModel::edited);
    QSignalSpy validated(m.get(), &QueueTableModel::validated);
    QVERIFY(m->set_conditionals({0, 1}, {"default_unknown"}));
    QCOMPARE(edited.count(), 1);
    QVERIFY(validated.count() >= 1);
    const pychron::experiment::ConditionalRef ref{"default_unknown", "action"};
    QCOMPARE(m->queue().runs[0].conditionals, std::vector{ref});
    QCOMPARE(m->queue().runs[1].conditionals, std::vector{ref});
    QVERIFY(m->queue().runs[2].conditionals.empty());
    QVERIFY(m->runnable());
    QVERIFY(m->set_conditionals({0}, {}));
    QVERIFY(m->queue().runs[0].conditionals.empty());
    QVERIFY(!m->set_conditionals({}, {"default_unknown"}));
    QVERIFY(!m->set_conditionals({9}, {"default_unknown"}));

    // A file the lab does not have is the row's error, as with a typed name.
    QVERIFY(m->set_conditionals({2}, {"no_such_file"}));
    QVERIFY(m->row_has_error(2));
  }

  void setConditionalsKeepsKind() {
    auto m = model();
    QueueSpec q = m->queue();
    q.runs[0].conditionals = {{"system", "truncate"}};
    m->set_queue(q);
    QVERIFY(m->set_conditionals({0}, {"system", "default_unknown"}));
    using Ref = pychron::experiment::ConditionalRef;
    QCOMPARE(m->queue().runs[0].conditionals, (std::vector<Ref>{{"system", "truncate"}, {"default_unknown", "action"}}));
  }

  void setConditionalsKeepsAMissingReference() {
    auto m = model();
    QueueSpec q = m->queue();
    q.runs[0].conditionals = {{"gone", "truncate"}};
    m->set_queue(q);
    QSignalSpy edited(m.get(), &QueueTableModel::edited);
    QVERIFY(m->set_conditionals({0}, {"gone"}));  // unchanged: accepted, nothing to do
    QCOMPARE(edited.count(), 0);
    QCOMPARE(cell(*m, 0, QueueTableModel::Conditionals), QStringLiteral("gone"));
    using Ref = pychron::experiment::ConditionalRef;
    QCOMPARE(m->queue().runs[0].conditionals, (std::vector<Ref>{{"gone", "truncate"}}));
  }

  void setConditionalsFollowsTheFrozenRows() {
    auto m = model();
    int offered = 0;
    m->set_live(1, [&](std::uint64_t base, const QueueSpec&) -> pychron::Result<std::uint64_t> {
      ++offered;
      return base + 1;
    });
    QVERIFY(!m->set_conditionals({0}, {"default_unknown"}));
    QVERIFY(!m->set_conditionals({0, 1}, {"default_unknown"}));  // all or nothing
    QVERIFY(m->queue().runs[0].conditionals.empty());
    QVERIFY(m->queue().runs[1].conditionals.empty());
    QCOMPARE(offered, 0);
    QVERIFY(m->set_conditionals({1, 2}, {"default_unknown"}));
    QCOMPARE(offered, 1);
    QCOMPARE(m->queue().runs[2].conditionals.size(), std::size_t{1});
    m->end_live();
    m->set_locked(true);
    QVERIFY(!m->set_conditionals({1}, {}));
  }

  void setConditionalsPerRowIsOneEdit() {
    auto m = model();
    int offered = 0;
    bool accept = true;
    m->set_live(0, [&](std::uint64_t base, const QueueSpec&) -> pychron::Result<std::uint64_t> {
      ++offered;
      if (!accept) return pychron::fail(pychron::ErrorKind::Config, "the queue changed");
      return base + 1;
    });
    QSignalSpy edited(m.get(), &QueueTableModel::edited);
    QVERIFY(m->set_conditionals({{0, {"system"}}, {1, {"system", "default_unknown"}}, {2, {}}}));
    QCOMPARE(offered, 1);
    QCOMPARE(edited.count(), 1);
    QCOMPARE(m->queue().runs[0].conditionals.size(), std::size_t{1});
    QCOMPARE(m->queue().runs[1].conditionals.size(), std::size_t{2});

    // Refused by the executor: no row changes.
    accept = false;
    QVERIFY(!m->set_conditionals({{0, {}}, {1, {}}}));
    QCOMPARE(m->queue().runs[0].conditionals.size(), std::size_t{1});
    QCOMPARE(m->queue().runs[1].conditionals.size(), std::size_t{2});
    QVERIFY(!m->set_conditionals({{0, {}}, {7, {}}}));  // a row that is not there
  }

  void setQueueConditionals() {
    auto m = model();
    QSignalSpy edited(m.get(), &QueueTableModel::edited);
    QVERIFY(m->set_queue_conditionals("default_unknown"));
    QCOMPARE(m->queue().queue_conditionals, std::string("default_unknown"));
    QCOMPARE(edited.count(), 1);
    QVERIFY(m->set_queue_conditionals("default_unknown"));  // unchanged
    QCOMPARE(edited.count(), 1);
    QCOMPARE(m->rowCount(), 3);

    QVERIFY(m->set_queue_conditionals("no_such_file"));
    QVERIFY(!m->runnable());
    QVERIFY(!m->queue_diagnostics().isEmpty());
    QVERIFY(m->set_queue_conditionals(""));
    QVERIFY(m->runnable());

    m->set_live(0, [](std::uint64_t base, const QueueSpec&) -> pychron::Result<std::uint64_t> { return base + 1; });
    QVERIFY(!m->set_queue_conditionals("default_unknown"));
    QVERIFY(m->queue().queue_conditionals.empty());
    m->end_live();
    m->set_locked(true);
    QVERIFY(!m->set_queue_conditionals("default_unknown"));
  }

  void statusFollowsRunEvents() {
    auto m = model();
    m->on_run_started(exec::RunStarted{1, "uuid-1", "66001", {}});
    QCOMPARE(cell(*m, 1, QueueTableModel::Status), QStringLiteral("preparing"));
    m->on_run_state(pychron::experiment::run::RunStateChanged{"uuid-1", RunState::Preparing, RunState::Measuring, {}, {}});
    QCOMPARE(cell(*m, 1, QueueTableModel::Status), QStringLiteral("measuring"));
    QCOMPARE(m->data(m->index(1, QueueTableModel::Status), Qt::BackgroundRole).value<QBrush>().color(),
             QueueTableModel::state_color(RunState::Measuring));
    m->on_run_state(pychron::experiment::run::RunStateChanged{"other", RunState::Preparing, RunState::Failed, {}, {}});
    QCOMPARE(cell(*m, 1, QueueTableModel::Status), QStringLiteral("measuring"));

    exec::RunSummary s;
    s.row = 1;
    s.run_id = "uuid-1";
    s.identifier = "66001";
    s.aliquot = 7;
    s.state = RunState::Success;
    s.truncated = true;
    m->on_run_finished(exec::RunFinished{s});
    QCOMPARE(cell(*m, 1, QueueTableModel::Status), QStringLiteral("success (truncated)"));
    QCOMPARE(cell(*m, 1, QueueTableModel::Aliquot), QStringLiteral("7"));
    QCOMPARE(m->data(m->index(1, QueueTableModel::Status), Qt::BackgroundRole).value<QBrush>().color(),
             QueueTableModel::state_color(RunState::Success));

    s.row = 2;
    s.run_id = "uuid-2";
    s.state = RunState::Failed;
    s.truncated = false;
    s.error = "main: no reading";
    m->on_run_finished(exec::RunFinished{s});
    QCOMPARE(cell(*m, 2, QueueTableModel::Status), QStringLiteral("failed"));
    QVERIFY(m->data(m->index(2, QueueTableModel::Status), Qt::ToolTipRole).toString().contains(QStringLiteral("no reading")));

    // A queue snapshot from the executor keeps the statuses; a new queue drops them.
    auto spec = m->queue();
    spec.runs.push_back(spec.runs[0]);
    m->set_queue(spec, true);
    QCOMPARE(m->rowCount(), 4);
    QCOMPARE(cell(*m, 1, QueueTableModel::Status), QStringLiteral("success (truncated)"));
    m->set_queue(spec);
    QVERIFY(cell(*m, 1, QueueTableModel::Status).isEmpty());
  }
};

QTEST_MAIN(TestQueueTableModel)
#include "test_queue_table_model.moc"
