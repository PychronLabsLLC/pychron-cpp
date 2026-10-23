// ExecutedProxy and QueueProxy over one QueueTableModel (queue-tables design 3),
// against the example lab (no hardware: only the lab files).

#include <filesystem>
#include <memory>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "queue_table_model.hpp"
#include "queue_views.hpp"

using pychron::experiment::QueueSpec;
using pychron::experiment::run::RunState;
using pychron::ui::ExecutedProxy;
using pychron::ui::QueueProxy;
using pychron::ui::QueueTableModel;
namespace exec = pychron::experiment::executor;
namespace lab = pychron::experiment::lab;

class TestQueueViews : public QObject {
  Q_OBJECT

  std::filesystem::path dir_;
  std::unique_ptr<lab::Lab> lab_;

  std::unique_ptr<QueueTableModel> model() {
    auto m = std::make_unique<QueueTableModel>(lab_->ids, [this](const QueueSpec& q) { return lab::check_lab_queue(*lab_, q); });
    m->set_queue(pychron::ui::test::example_queue(dir_, lab_->ids));
    return m;
  }
  static void go_live(QueueTableModel& m) {
    m.set_live(0, [](std::uint64_t base, const QueueSpec&) -> pychron::Result<std::uint64_t> { return base + 1; });
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

  void theBoundarySplitsTheRows() {
    auto m = model();
    ExecutedProxy executed(*m);
    QueueProxy queue(*m);
    QCOMPARE(m->rowCount(), 3);
    QCOMPARE(executed.rowCount(), 0);
    QCOMPARE(queue.rowCount(), 3);
    QCOMPARE(executed.columnCount(), int(QueueTableModel::Count));

    go_live(*m);
    m->set_frozen(1);
    QCOMPARE(executed.rowCount(), 1);
    QCOMPARE(queue.rowCount(), 2);
    QCOMPARE(queue.mapToSource(queue.index(0, 0)).row(), 1);
    QCOMPARE(executed.mapToSource(executed.index(0, 0)).row(), 0);
    // The # column keeps the queue's numbers in both views.
    QCOMPARE(queue.data(queue.index(0, QueueTableModel::Row)).toString(), QStringLiteral("2"));

    m->set_frozen(3);
    QCOMPARE(executed.rowCount(), 3);
    QCOMPARE(queue.rowCount(), 0);

    // A new queue has nothing executed.
    m->end_live();
    m->set_queue(pychron::ui::test::example_queue(dir_, lab_->ids));
    QCOMPARE(executed.rowCount(), 0);
    QCOMPARE(queue.rowCount(), 3);
  }

  void executedRowsAreNotEditable() {
    auto m = model();
    ExecutedProxy executed(*m);
    QueueProxy queue(*m);
    QVERIFY(m->resume_from(1, {}));
    QCOMPARE(executed.rowCount(), 1);
    QVERIFY(!(executed.flags(executed.index(0, QueueTableModel::Identifier)) & Qt::ItemIsEditable));
    QVERIFY(executed.flags(executed.index(0, QueueTableModel::Identifier)) & Qt::ItemIsSelectable);
    QVERIFY(queue.flags(queue.index(0, QueueTableModel::Identifier)) & Qt::ItemIsEditable);
  }

  void aStatusChangeDoesNotRefilter() {
    auto m = model();
    go_live(*m);
    m->set_frozen(1);
    ExecutedProxy executed(*m);
    QueueProxy queue(*m);
    std::vector<std::unique_ptr<QSignalSpy>> structure;
    for (QAbstractItemModel* proxy : {static_cast<QAbstractItemModel*>(&executed), static_cast<QAbstractItemModel*>(&queue)}) {
      structure.push_back(std::make_unique<QSignalSpy>(proxy, &QAbstractItemModel::layoutChanged));
      structure.push_back(std::make_unique<QSignalSpy>(proxy, &QAbstractItemModel::rowsInserted));
      structure.push_back(std::make_unique<QSignalSpy>(proxy, &QAbstractItemModel::rowsRemoved));
      structure.push_back(std::make_unique<QSignalSpy>(proxy, &QAbstractItemModel::modelReset));
    }
    QSignalSpy changed(&executed, &QAbstractItemModel::dataChanged);

    m->on_run_started(exec::RunStarted{0, "uuid-0", "bu", "bu-01", {}});
    exec::RunSummary s;
    s.row = 0;
    s.run_id = "uuid-0";
    s.state = RunState::Success;
    m->on_run_finished(exec::RunFinished{s});

    for (const auto& spy : structure) QCOMPARE(spy->count(), 0);
    QVERIFY(changed.count() > 0);
    QCOMPARE(executed.rowCount(), 1);
    QCOMPARE(executed.data(executed.index(0, QueueTableModel::Status)).toString(), QStringLiteral("success"));
  }

  // Review focus 1: the executor's own edit resets the model; both views stay
  // split at the boundary it keeps.
  void aQueueEditKeepsBothViewsSplit() {
    auto m = model();
    ExecutedProxy executed(*m);
    QueueProxy queue(*m);
    go_live(*m);
    m->set_frozen(2);
    exec::QueueEdited e;
    e.queue = m->queue();
    e.version = 9;
    e.frozen = 2;
    QVERIFY(m->on_queue_edited(e));
    QCOMPARE(executed.rowCount(), 2);
    QCOMPARE(queue.rowCount(), 1);
    QCOMPARE(queue.mapToSource(queue.index(0, 0)).row(), 2);
  }

  // Edits in the Queue view, Reset and Clear executed keep the split right.
  void operationsKeepTheSplit() {
    auto m = model();
    ExecutedProxy executed(*m);
    QueueProxy queue(*m);
    QVERIFY(m->resume_from(1, {{0, RunState::Success}}));
    QVERIFY(m->repeat({0}));
    QCOMPARE(executed.rowCount(), 1);
    QCOMPARE(queue.rowCount(), 3);
    QVERIFY(m->reset_executed());
    QCOMPARE(executed.rowCount(), 0);
    QCOMPARE(queue.rowCount(), 4);
    QVERIFY(m->resume_from(2, {}));
    QVERIFY(m->clear_executed());
    QCOMPARE(executed.rowCount(), 0);
    QCOMPARE(queue.rowCount(), 2);
  }
};

QTEST_MAIN(TestQueueViews)
#include "test_queue_views.moc"
