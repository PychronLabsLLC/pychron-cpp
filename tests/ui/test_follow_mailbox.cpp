// FollowMailbox: the followed steps waiting for the main thread (follow plots
// design, section 4.4). Needs no event loop.

#include <QtTest/QtTest>

#include "follow_mailbox.hpp"

using namespace pychron;
using namespace pychron::ui;

class FollowMailboxTest : public QObject {
  Q_OBJECT

 private slots:
  void a_stalled_main_thread_keeps_the_newest() {
    FollowMailbox box;
    int asks = 0;
    for (int i = 0; i < 5000; ++i) {
      laser::FollowSample sample;
      sample.step.elapsed_s = i;
      if (box.add(sample)) {
        ++asks;
        QCOMPARE(i, 0);  // only the first add asks for a post
      }
    }
    QCOMPARE(asks, 1);
    const auto taken = box.take();
    QCOMPARE(taken.size(), FollowMailbox::kMaxPending);
    QCOMPARE(taken.back().step.elapsed_s, 4999.0);
    QCOMPARE(taken.front().step.elapsed_s, 5000.0 - static_cast<double>(FollowMailbox::kMaxPending));
  }

  void taking_lets_the_next_add_ask_again() {
    FollowMailbox box;
    QVERIFY(box.add({}));
    QVERIFY(!box.add({}));
    QCOMPARE(box.take().size(), std::size_t{2});
    QVERIFY(box.take().empty());
    QVERIFY(box.add({}));
  }
};

QTEST_GUILESS_MAIN(FollowMailboxTest)
#include "test_follow_mailbox.moc"
