// NoDatabasePersister: what a session saves through when its database could
// not be opened. No queue starts, and the reason is said.

#include <cstdint>
#include <string>
#include <vector>

#include <QtTest/QtTest>

#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "no_database_persister.hpp"

using pychron::ErrorKind;
using pychron::ui::NoDatabasePersister;

class TestNoDatabasePersister : public QObject {
  Q_OBJECT

 private slots:
  void itRefusesEveryQueueWithTheReason() {
    NoDatabasePersister persister("unable to open database file");
    const auto problems = persister.check_queue({});
    QVERIFY(problems.has_value());
    QCOMPARE(problems->size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(problems->front()),
             QStringLiteral("the database cannot be reached: unable to open database file"));
    QCOMPARE(QString::fromStdString(persister.why()),
             QStringLiteral("the database cannot be reached: unable to open database file"));
  }

  void nothingIsSavedThroughIt() {
    NoDatabasePersister persister("unable to open database file");
    const auto refused = [](const pychron::Error& error) {
      return error.kind == ErrorKind::Io &&
             error.what == "the database cannot be reached: unable to open database file";
    };
    const auto aliquot = persister.next_aliquot("66001");
    QVERIFY(!aliquot.has_value() && refused(aliquot.error()));
    const auto begun = persister.begin_run({}, {});
    QVERIFY(!begun.has_value() && refused(begun.error()));
    const auto extraction = persister.save_extraction({});
    QVERIFY(!extraction.has_value() && refused(extraction.error()));
    const auto analysis = persister.save_analysis({});
    QVERIFY(!analysis.has_value() && refused(analysis.error()));
    const auto artifact = persister.save_artifact("uuid", "frame.png", std::vector<std::uint8_t>{1, 2});
    QVERIFY(!artifact.has_value() && refused(artifact.error()));
    QVERIFY(persister.flush().has_value());
  }

  // As the session uses it: the queue does not start, and the error is the
  // one the operator reads.
  void aSessionOnItStartsNoQueue() {
    NoDatabasePersister persister("unable to open database file");
    pychron::ui::test::SimLab sim({}, {}, {}, &persister);
    pychron::ui::ExperimentBridge bridge(*sim.session, sim.line->bus());
    const auto started = bridge.start(sim.queue(), 0);
    QVERIFY(!started.has_value());
    QCOMPARE(started.error().kind, ErrorKind::Config);
    QCOMPARE(QString::fromStdString(started.error().what),
             QStringLiteral("the queue cannot be saved:\nthe database cannot be reached: unable to open database file"));
    QVERIFY(!bridge.running());
  }
};

QTEST_MAIN(TestNoDatabasePersister)
#include "test_no_database_persister.moc"
