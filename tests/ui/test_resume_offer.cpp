// resume_offer (queue-tables design 5): the executor's state file set against
// the open queue. A pure function: no lab, no window.

#include <cstddef>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QtTest/QtTest>

#include "resume_offer.hpp"

using pychron::experiment::QueueSpec;
using pychron::experiment::RunSpec;
using pychron::experiment::executor::ExecutorStateFile;
using pychron::experiment::executor::ExecutorStateRun;
using pychron::experiment::run::RunState;
using pychron::ui::resume_offer;

namespace {

using Statuses = std::vector<std::pair<std::size_t, RunState>>;

QueueSpec three_rows() {
  QueueSpec q;
  q.name = "q";
  for (const char* id : {"a", "b", "c"}) {
    RunSpec r;
    r.id.identifier = id;
    q.runs.push_back(r);
  }
  return q;
}

ExecutorStateFile state(std::string queue, std::string executor_state, std::size_t next_row,
                        std::vector<ExecutorStateRun> runs) {
  ExecutorStateFile f;
  f.queue = std::move(queue);
  f.state = std::move(executor_state);
  f.next_row = next_row;
  f.runs = std::move(runs);
  return f;
}

QDateTime written() { return {QDate(2026, 10, 17), QTime(3, 4)}; }

}  // namespace

class TestResumeOffer : public QObject {
  Q_OBJECT

 private slots:
  void offeredForAWorkingState() {
    const auto offer = resume_offer(state("q", "running", 1, {{0, "a", "r1", "success"}}), three_rows(), written());
    QVERIFY(offer.has_value());
    QCOMPARE(offer->next_row, std::size_t{1});
    QVERIFY2(offer->question.contains(QStringLiteral("row 2 (b)")), qPrintable(offer->question));
    QCOMPARE(offer->question,
             QStringLiteral("The queue q stopped at row 2 (b) at 2026-10-17 03:04. Resume from row 2?"));
    QCOMPARE(offer->how, QStringLiteral("It did not finish (the program ended while it ran)."));
    QVERIFY(offer->resumable());
    QVERIFY(offer->statuses == (Statuses{{0, RunState::Success}}));
  }

  void offeredAfterAStop() {
    for (const char* s : {"finalizing", "idle"}) {
      const auto offer = resume_offer(state("q", s, 1, {{0, "a", "r1", "success"}}), three_rows(), written());
      QVERIFY(offer.has_value());
      QCOMPARE(offer->how, QStringLiteral("It was stopped."));
      QVERIFY(offer->resumable());
    }
  }

  void notOfferedForAnotherQueueOrRow() {
    QVERIFY(!resume_offer(state("other", "running", 1, {}), three_rows(), written()));
    for (std::size_t row : {std::size_t{0}, std::size_t{3}, std::size_t{4}})
      QVERIFY(!resume_offer(state("q", "running", row, {}), three_rows(), written()));
    // Nothing in runs[] to compare: resumable.
    const auto offer = resume_offer(state("q", "running", 2, {}), three_rows(), written());
    QVERIFY(offer.has_value());
    QVERIFY(offer->resumable());
    QVERIFY(offer->statuses.empty());
  }

  void anIdentifierThatChangedDisablesIt() {
    const auto offer = resume_offer(state("q", "running", 1, {{0, "x", "r1", "success"}}), three_rows(), written());
    QVERIFY(offer.has_value());
    QVERIFY(!offer->resumable());
    QCOMPARE(offer->mismatch, QStringLiteral("Row 1 is a now; the file has x."));

    // A row past the queue's end cannot match.
    const auto past = resume_offer(state("q", "running", 1, {{5, "z", "r1", "success"}}), three_rows(), written());
    QVERIFY(past.has_value());
    QVERIFY(!past->resumable());
  }

  void theLastEntryForARowWins() {
    const auto offer = resume_offer(
        state("q", "running", 2, {{0, "a", "r1", "failed"}, {0, "a", "r2", "success"}, {1, "b", "r3", "bogus"}}),
        three_rows(), written());
    QVERIFY(offer.has_value());
    QVERIFY(offer->resumable());
    QVERIFY(offer->statuses == (Statuses{{0, RunState::Success}}));
  }
};

QTEST_GUILESS_MAIN(TestResumeOffer)
#include "test_resume_offer.moc"
