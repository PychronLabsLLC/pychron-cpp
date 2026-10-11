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

  // n runs of 1500 s each: a 10 s wait, then preparing, extracting, a pump
  // wait, measuring, post-measuring, success. Run i is "r<i>".
  static void feed_long(TimelineModel& m, int n) {
    for (int i = 0; i < n; ++i) {
      const double b = i * 1500.0;
      const std::string id = "r" + std::to_string(i);
      m.on_waiting({"delay between analyses", std::chrono::duration<double>(10), at(b), {}});
      m.on_run_started({static_cast<std::size_t>(i), id, "66001", at(b + 10)});
      m.on_state(state(id.c_str(), RunState::Pending, RunState::Preparing, b + 10));
      m.on_state(state(id.c_str(), RunState::Preparing, RunState::Extracting, b + 20));
      m.on_waiting({"minimum pump time", std::chrono::duration<double>(30), at(b + 100), id});
      m.on_state(state(id.c_str(), RunState::Extracting, RunState::Measuring, b + 300));
      m.on_state(state(id.c_str(), RunState::Measuring, RunState::PostMeasuring, b + 1300));
      m.on_state(state(id.c_str(), RunState::PostMeasuring, RunState::Success, b + 1480));
    }
  }

 private slots:
  void aThousandRunsLeaveNothingOpen() {
    TimelineModel m;
    feed_long(m, 1000);
    QCOMPARE(m.segments().size(), std::size_t{6000});
    QCOMPARE(m.open_count(), std::size_t{0});
    for (const auto& s : m.segments()) QVERIFY(s.end.has_value());
  }

  void runStartsAreKept() {
    TimelineModel m;
    feed_long(m, 10);
    QCOMPARE(m.run_starts().size(), std::size_t{10});
    QVERIFY(std::ranges::is_sorted(m.run_starts()));
    QCOMPARE(m.run_starts()[3], 3 * 1500.0 + 10);
    m.clear();
    QVERIFY(m.run_starts().empty());
  }

  void visibleIsTheWindowsSegments() {
    TimelineModel m;
    feed_long(m, 1000);
    const double t0 = 999 * 1500.0, t1 = t0 + 1500;
    const auto shown = m.visible(t0, t1);
    QVERIFY(!shown.empty());
    QVERIFY(shown.size() < 100);
    QVERIFY(std::ranges::is_sorted(shown));
    std::size_t of_last = 0;
    for (std::size_t i : shown) {
      const auto& s = m.segments()[i];
      QVERIFY(s.start <= t1);
      QVERIFY(m.drawn_end(s) >= t0);
      if (s.run_id == "r999") ++of_last;
    }
    QCOMPARE(of_last, std::size_t{5});  // four states and the pump wait
    // the same answer as looking at every segment
    std::vector<std::size_t> all;
    for (std::size_t i = 0; i < m.segments().size(); ++i)
      if (m.segments()[i].start <= t1 && m.drawn_end(m.segments()[i]) >= t0) all.push_back(i);
    QCOMPARE(shown, all);
    QVERIFY(m.visible(-100, -1).empty());
  }

  void visibleIncludesOpenSegments() {
    TimelineModel m;
    feed(m);  // leaves B extracting and C preparing
    m.advance(at(10000));
    const auto shown = m.visible(9000, 10000);
    QCOMPARE(shown.size(), m.open_count());
    QVERIFY(m.open_count() >= 2);
    for (std::size_t i : shown) QVERIFY(!m.segments()[i].end.has_value());
  }

  // Events that arrive out of order are still found.
  void visibleSurvivesAStartThatGoesBackwards() {
    TimelineModel m;
    m.on_run_started({0, "A", "66001", at(50)});
    m.on_state(state("A", RunState::Pending, RunState::Preparing, 50));
    m.on_waiting({"late", std::chrono::duration<double>(5), at(10), {}});
    m.on_queue_ended(at(100));
    QCOMPARE(m.visible(0, 20).size(), std::size_t{1});
    QCOMPARE(m.visible(40, 100).size(), std::size_t{1});
  }

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
