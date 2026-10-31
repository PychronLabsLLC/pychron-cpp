// FollowPlotModel: the lines behind the follow plots, no widgets.

#include <cmath>
#include <numbers>
#include <optional>

#include <QtTest/QtTest>

#include "follow_plot_model.hpp"

using namespace pychron;
using pychron::laser::FollowSample;
using pychron::ui::FollowLine;
using pychron::ui::FollowPlotModel;
using pychron::vision::DragonflyStep;

namespace {

constexpr double kPi = std::numbers::pi;

// stage (1,2), a blob at (0.3,0.4) of area pi, saturation 0.5; with a grain at
// (3,4), beam at (0,0), brightness 0.7, radius 0.5.
FollowSample sample(double t, bool with_grain = true) {
  FollowSample s;
  s.step.stage_mm = {1, 2};
  s.step.elapsed_s = t;
  s.step.saturation = 0.5;
  s.step.reason = DragonflyStep::Reason::Track;
  s.step.seen = vision::DragonflySeen{{0.3, 0.4}, kPi};
  if (with_grain) {
    laser::GrainView g;
    g.grain_mm = {3, 4};
    g.beam_mm = {0, 0};
    g.truth.brightness = 0.7;
    g.truth.radius_mm = 0.5;
    s.grain = g;
  }
  return s;
}

double last(const FollowPlotModel& m, FollowLine line) {
  return m.series(line).back().value;
}

}  // namespace

class FollowPlotModelTest : public QObject {
  Q_OBJECT
 private slots:
  void a_sample_fills_its_lines() {
    FollowPlotModel m;
    m.append(sample(2.0));
    QCOMPARE(last(m, FollowLine::StageX), 1.0);
    QCOMPARE(last(m, FollowLine::StageY), 2.0);
    QCOMPARE(last(m, FollowLine::GrainX), 3.0);
    QCOMPARE(last(m, FollowLine::GrainY), 4.0);
    QVERIFY(qFuzzyCompare(last(m, FollowLine::DeviationSeen), 0.5));
    QVERIFY(qFuzzyCompare(last(m, FollowLine::SizeSeen), 2.0));
    QCOMPARE(last(m, FollowLine::SizeTrue), 1.0);
    QCOMPARE(last(m, FollowLine::LuminositySeen), 0.5);
    QCOMPARE(last(m, FollowLine::LuminosityTrue), 0.7);
    QCOMPARE(m.latest_x(), 2.0);
    QCOMPARE(m.series(FollowLine::StageX).back().t, 2.0);
  }

  void the_true_deviation_is_from_the_beam() {
    FollowPlotModel m;
    m.append(sample(0.0));
    QCOMPARE(last(m, FollowLine::DeviationTrue), 5.0);
  }

  void no_blob_breaks_the_seen_lines() {
    FollowPlotModel m;
    FollowSample s = sample(0.0);
    s.step.seen.reset();
    s.step.reason = DragonflyStep::Reason::Saturated;
    m.append(s);
    QVERIFY(std::isnan(last(m, FollowLine::DeviationSeen)));
    QVERIFY(std::isnan(last(m, FollowLine::SizeSeen)));
    QVERIFY(std::isfinite(last(m, FollowLine::LuminositySeen)));
  }

  void a_miss_breaks_luminosity() {
    for (auto reason : {DragonflyStep::Reason::Miss, DragonflyStep::Reason::Search,
                        DragonflyStep::Reason::Elapsed, DragonflyStep::Reason::Invalid}) {
      FollowPlotModel m;
      FollowSample s = sample(0.0);
      s.step.reason = reason;
      m.append(s);
      QVERIFY(std::isnan(last(m, FollowLine::LuminositySeen)));
    }
  }

  void a_real_laser_has_no_true_lines() {
    FollowPlotModel m;
    m.append(sample(0.0, false));
    m.append(sample(1.0, false));
    for (auto line : {FollowLine::GrainX, FollowLine::GrainY, FollowLine::DeviationTrue,
                      FollowLine::LuminosityTrue, FollowLine::SizeTrue}) {
      QVERIFY(!m.has(line));
    }
    QVERIFY(m.has(FollowLine::StageX));
    QVERIFY(m.has(FollowLine::DeviationSeen));
  }

  void a_point_grain_has_no_true_size() {
    FollowPlotModel m;
    FollowSample s = sample(0.0);
    s.grain->truth.radius_mm = 0;
    m.append(s);
    QVERIFY(std::isnan(last(m, FollowLine::SizeTrue)));
    QVERIFY(!m.has(FollowLine::SizeTrue));
    QVERIFY(m.has(FollowLine::LuminosityTrue));
  }

  void an_earlier_time_starts_a_new_run() {
    FollowPlotModel m;
    m.append(sample(5.0));
    m.append(sample(1.0));
    QCOMPARE(m.series(FollowLine::StageX).size(), std::size_t{1});
    QCOMPARE(m.latest_x(), 1.0);
  }

  void one_row_is_a_run() {
    FollowPlotModel m;
    m.append(sample(0.0));
    QVERIFY(!m.series(FollowLine::StageX).empty());
    const auto r = m.series(FollowLine::StageX).range(-1, 1);
    QVERIFY(r.has_value());
    QVERIFY(std::isfinite(r->first) && std::isfinite(r->second));
  }

  void the_latest_row_gives_the_fence() {
    FollowPlotModel m;
    QVERIFY(!m.fence().has_value());
    FollowSample s = sample(0.0);
    m.append(s);
    QVERIFY(!m.fence().has_value());  // no perimeter: no fence
    s.step.elapsed_s = 1;
    s.step.start_mm = {7, 8};
    s.step.perimeter_mm = 0.5;
    m.append(s);
    QVERIFY(m.fence().has_value());
    QCOMPARE(m.fence()->x, 7.0);
    QCOMPARE(m.fence()->y, 8.0);
    QCOMPARE(m.fence()->radius_mm, 0.5);
    s.step.elapsed_s = 2;
    s.step.perimeter_mm = 0.25;
    m.append(s);
    QCOMPARE(m.fence()->radius_mm, 0.25);
    m.clear();
    QVERIFY(!m.fence().has_value());
  }

  void clear_empties_the_lines() {
    FollowPlotModel m;
    m.append(sample(3.0));
    m.clear();
    QVERIFY(m.series(FollowLine::StageX).empty());
    QVERIFY(!m.has(FollowLine::StageX));
    QCOMPARE(m.latest_x(), 0.0);
  }
};

QTEST_APPLESS_MAIN(FollowPlotModelTest)
#include "test_follow_plot_model.moc"
