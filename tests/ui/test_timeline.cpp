// The phase timeline: lanes for overlapped runs, state segments, waits.

#include <QtTest/QtTest>

#include "timeline.hpp"

using pychron::TimePoint;
using pychron::experiment::run::RunState;
using pychron::experiment::run::RunStateChanged;
using pychron::ui::TimelineModel;
using pychron::ui::TimelineSegment;
using pychron::ui::TimelineView;

namespace {

const TimePoint kT0 = TimePoint{} + std::chrono::hours(1);
TimePoint at(double s) { return kT0 + std::chrono::duration_cast<pychron::Duration>(std::chrono::duration<double>(s)); }

RunStateChanged state(const char* id, RunState from, RunState to, double t) { return {id, from, to, {}, at(t)}; }

std::vector<const TimelineSegment*> of(const TimelineModel& m, const std::string& run) {
  std::vector<const TimelineSegment*> out;
  for (const auto& s : m.segments())
    if (s.run_id == run && s.state) out.push_back(&s);
  return out;
}

}  // namespace

class TestTimeline : public QObject {
  Q_OBJECT

  // A queue wait, run A, run B overlapping A, then run C reusing A's lane.
  static void feed(TimelineModel& m) {
    m.on_waiting({"delay before 66001", std::chrono::duration<double>(5), at(0), {}});
    m.on_run_started({0, "A", "66001", at(5)});
    m.on_state(state("A", RunState::Pending, RunState::Preparing, 5));
    m.on_state(state("A", RunState::Preparing, RunState::Extracting, 6));
    m.on_state(state("A", RunState::Extracting, RunState::Measuring, 20));
    // B starts while A measures (overlap) and waits for the extraction device.
    m.on_run_started({1, "B", "66002", at(30)});
    m.on_state(state("B", RunState::Pending, RunState::Preparing, 30));
    m.on_waiting({"extraction device", {}, at(31), "B"});
    m.on_state(state("B", RunState::Preparing, RunState::Extracting, 33));
    m.on_state(state("A", RunState::Measuring, RunState::PostMeasuring, 40));
    m.on_state(state("A", RunState::PostMeasuring, RunState::Success, 45));
    m.on_run_started({2, "C", "66003", at(70)});
    m.on_state(state("C", RunState::Pending, RunState::Preparing, 70));
  }

 private slots:
  void overlappedRunsTakeAnotherLane() {
    TimelineModel m;
    feed(m);
    QCOMPARE(m.run_lanes(), 2);
    QCOMPARE(m.lane_of("A"), std::optional<int>(1));
    QCOMPARE(m.lane_of("B"), std::optional<int>(2));
    QCOMPARE(m.lane_of("C"), std::optional<int>(1));  // A's lane was free again

    const auto a = of(m, "A");
    QCOMPARE(a.size(), std::size_t{4});
    QCOMPARE(*a[0]->state, RunState::Preparing);
    QCOMPARE(a[0]->start, 5.0);
    QCOMPARE(a[0]->end, std::optional<double>(6.0));
    QCOMPARE(*a[2]->state, RunState::Measuring);
    QCOMPARE(a[2]->end, std::optional<double>(40.0));
    QCOMPARE(a[3]->end, std::optional<double>(45.0));  // the final state closed post-measuring
    QCOMPARE(a[2]->label, QStringLiteral("66001"));
  }

  void waitsLiveInTheirOwnLane() {
    TimelineModel m;
    feed(m);
    std::vector<const TimelineSegment*> waits;
    for (const auto& s : m.segments())
      if (!s.state) waits.push_back(&s);
    QCOMPARE(waits.size(), std::size_t{2});
    QCOMPARE(waits[0]->lane, 0);
    QCOMPARE(waits[0]->label, QStringLiteral("delay before 66001"));
    QCOMPARE(waits[0]->end, std::optional<double>(5.0));  // closed when A started
    QCOMPARE(waits[1]->label, QStringLiteral("66002: extraction device"));
    QCOMPARE(waits[1]->end, std::optional<double>(33.0));  // closed when B moved on
  }

  void openSegmentsGrowWithTheClock() {
    TimelineModel m;
    feed(m);
    const auto c = of(m, "C");
    QCOMPARE(c.size(), std::size_t{1});
    QVERIFY(!c[0]->end.has_value());
    QCOMPARE(m.drawn_end(*c[0]), 70.0);
    m.advance(at(90));
    QCOMPARE(m.now(), 90.0);
    QCOMPARE(m.drawn_end(*c[0]), 90.0);
    // A timed wait never draws past its planned end.
    m.on_waiting({"delay", std::chrono::duration<double>(10), at(91), {}});
    m.advance(at(120));
    QCOMPARE(m.drawn_end(m.segments().back()), 101.0);
    m.on_queue_ended(at(125));
    for (const auto& s : m.segments()) QVERIFY(s.end.has_value());
    QCOMPARE(m.segments().back().end, std::optional<double>(101.0));
    m.clear();
    QVERIFY(m.segments().empty());
    QCOMPARE(m.run_lanes(), 0);
  }

  void viewHitsSegments() {
    TimelineModel m;
    feed(m);
    m.advance(at(100));
    TimelineView v(m);
    v.resize(800, v.sizeHint().height());
    QCOMPARE(v.sizeHint().height(), 16 + 20 * 3 + 4);  // waits + two lanes
    const auto a = of(m, "A");
    const QRectF r = v.segment_rect(*a[2]);  // A measuring, 20..40 of 100 s
    QVERIFY(r.width() > 100);
    const TimelineSegment* hit = v.segment_at(r.center().toPoint());
    QVERIFY(hit != nullptr);
    QCOMPARE(*hit->state, RunState::Measuring);
    QVERIFY(v.segment_at(QPoint(2, 2)) == nullptr);  // the lane labels
    v.show();
    QTest::qWait(10);  // paints without trouble
  }
};

QTEST_MAIN(TestTimeline)
#include "test_timeline.moc"
