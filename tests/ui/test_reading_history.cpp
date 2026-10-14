// A history of timed readings (pure data): a series a name, a day long.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QtTest/QtTest>

#include "reading_history.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using pychron::ui::ReadingHistory;

namespace {

TimePoint at(double t_s) {
  return TimePoint{} + std::chrono::duration_cast<Duration>(std::chrono::duration<double>(t_s));
}

const std::string kTorr = "torr";
constexpr Duration kDay = 24h;

std::vector<double> times(const ReadingHistory::Series& s) {
  std::vector<double> out;
  out.reserve(s.points.size());
  for (const auto& p : s.points) out.push_back(p.t);
  return out;
}

std::size_t breaks(const ReadingHistory::Series& s) {
  std::size_t n = 0;
  for (const auto& p : s.points) n += std::isnan(p.value) ? 1U : 0U;
  return n;
}

}  // namespace

class TestReadingHistory : public QObject {
  Q_OBJECT

 private slots:
  void aSeriesIsKeptForEachName() {
    ReadingHistory h({"ig", "cg"}, 3s);
    h.add("ig", 1e-8, kTorr, at(0));
    h.add("cg", 2e-3, kTorr, at(0.5));
    h.add("ig", 2e-8, kTorr, at(1));
    h.add("cg", 3e-3, kTorr, at(1.5));
    h.add("ig", 3e-8, kTorr, at(2));
    const auto ig = h.series("ig", kDay);
    const auto cg = h.series("cg", kDay);
    QCOMPARE(ig.points.size(), std::size_t{3});
    QCOMPARE(cg.points.size(), std::size_t{2});
    QCOMPARE(ig.points[2].value, 3e-8);
    QCOMPARE(cg.points[1].value, 3e-3);
    QCOMPARE(ig.units, QStringLiteral("torr"));
    QVERIFY(h.has("ig") && h.has("cg"));
  }

  void timesAreSecondsBeforeTheNewest() {
    ReadingHistory h({"ig"}, 100s);
    for (const double t : {100.0, 160.0, 220.0}) h.add("ig", 1.0, kTorr, at(t));
    const auto s = h.series("ig", kDay);
    QCOMPARE(times(s), (std::vector<double>{-120.0, -60.0, 0.0}));
    QVERIFY(s.newest.has_value());
    QCOMPARE(*s.newest, at(220));
    QCOMPARE(*h.oldest("ig"), at(100));
  }

  void aRangeLeavesOutWhatIsOlder() {
    ReadingHistory h({"ig"}, 100s);
    for (const double t : {100.0, 160.0, 220.0}) h.add("ig", 1.0, kTorr, at(t));
    QCOMPARE(times(h.series("ig", 90s)), (std::vector<double>{-60.0, 0.0}));
    QCOMPARE(times(h.series("ig", 60s)), (std::vector<double>{-60.0, 0.0}));  // the edge is in
    QCOMPARE(times(h.series("ig", 59s)), (std::vector<double>{0.0}));
    QCOMPARE(times(h.series("ig", 0s)), (std::vector<double>{0.0}));
  }

  void aLongSilenceIsABreak() {
    ReadingHistory h({"ig"}, 3s);
    for (const double t : {0.0, 1.0, 2.0, 10.0, 11.0}) h.add("ig", 5.0, kTorr, at(t));
    const auto s = h.series("ig", kDay);
    QCOMPARE(s.points.size(), std::size_t{6});
    QCOMPARE(breaks(s), std::size_t{1});
    QVERIFY(std::isnan(s.points[3].value));
    QCOMPARE(s.points[3].t, -5.0);  // midway between 2 and 10, seen from 11

    ReadingHistory joined({"ig"}, 3s);
    for (const double t : {0.0, 3.0, 6.0}) joined.add("ig", 5.0, kTorr, at(t));
    QCOMPARE(breaks(joined.series("ig", kDay)), std::size_t{0});
  }

  void timeGoingBackEmptiesThatSeries() {
    ReadingHistory h({"ig", "cg"}, 100s);
    h.add("ig", 1.0, kTorr, at(100));
    h.add("ig", 2.0, kTorr, at(160));
    h.add("cg", 7.0, kTorr, at(100));
    h.add("cg", 8.0, kTorr, at(160));
    const unsigned before = h.revision("ig");
    const unsigned other = h.revision("cg");
    h.add("ig", 3.0, kTorr, at(5));
    const auto s = h.series("ig", kDay);
    QCOMPARE(s.points.size(), std::size_t{1});
    QCOMPARE(s.points[0].value, 3.0);
    QCOMPARE(s.points[0].t, 0.0);
    QCOMPARE(*s.newest, at(5));
    QCOMPARE(*h.oldest("ig"), at(5));
    QVERIFY(h.revision("ig") != before);
    QCOMPARE(h.revision("cg"), other);
    QCOMPARE(h.series("cg", kDay).points.size(), std::size_t{2});
    // and it goes on from there
    h.add("ig", 4.0, kTorr, at(6));
    QCOMPARE(times(h.series("ig", kDay)), (std::vector<double>{-1.0, 0.0}));
  }

  void aChangeOfUnitsEmptiesThatSeries() {
    ReadingHistory h({"ig"}, 100s);
    h.add("ig", 1.0, "torr", at(0));
    h.add("ig", 2.0, "torr", at(1));
    h.add("ig", 2.7, "mbar", at(2));
    const auto s = h.series("ig", kDay);
    QCOMPARE(s.points.size(), std::size_t{1});
    QCOMPARE(s.points[0].value, 2.7);
    QCOMPARE(s.units, QStringLiteral("mbar"));
  }

  void nothingOlderThanADayIsKept() {
    ReadingHistory h({"ig"}, 2h);
    for (int hour = 0; hour <= 30; ++hour) h.add("ig", 1.0, kTorr, at(3600.0 * hour));
    const auto oldest = h.oldest("ig");
    QVERIFY(oldest.has_value());
    QVERIFY(at(3600.0 * 30) - *oldest <= kDay);
    QVERIFY(at(3600.0 * 30) - *oldest >= 23h);
    QCOMPARE(h.series("ig", 48h).points.size(), std::size_t{25});
  }

  void anUnknownNameIsAnEmptySeries() {
    const ReadingHistory h({"ig"}, 3s);
    const auto s = h.series("nope", kDay);
    QVERIFY(s.points.empty());
    QVERIFY(!s.newest.has_value());
    QVERIFY(s.units.isEmpty());
    QVERIFY(!h.has("nope"));
    QCOMPARE(h.revision("nope"), 0U);
    QVERIFY(!h.oldest("nope").has_value());
    // a name that is kept and has no reading yet
    QVERIFY(h.has("ig"));
    QVERIFY(h.series("ig", kDay).points.empty());
    QVERIFY(!h.series("ig", kDay).newest.has_value());
    QVERIFY(!h.oldest("ig").has_value());
  }

  void aReadingOfANameNotGivenIsIgnored() {
    ReadingHistory h({"ig"}, 3s);
    h.add("nope", 1.0, kTorr, at(0));
    QVERIFY(!h.has("nope"));
    QVERIFY(h.series("nope", kDay).points.empty());
    QCOMPARE(h.revision("nope"), 0U);
    QCOMPARE(h.revision("ig"), 0U);
  }

  void aValueThatIsNoNumberIsABreak() {
    ReadingHistory h({"ig"}, 100s);
    h.add("ig", 1.0, kTorr, at(0));
    h.add("ig", std::numeric_limits<double>::quiet_NaN(), kTorr, at(1));
    h.add("ig", std::numeric_limits<double>::infinity(), kTorr, at(2));
    h.add("ig", 2.0, kTorr, at(3));
    const auto s = h.series("ig", kDay);
    QCOMPARE(s.points.size(), std::size_t{4});
    QVERIFY(std::isnan(s.points[1].value));
    QVERIFY(std::isnan(s.points[2].value));
    QCOMPARE(s.points[3].value, 2.0);
    QCOMPARE(*s.newest, at(3));
  }

  void revisionMovesWithEachReading() {
    ReadingHistory h({"ig"}, 3s);
    QCOMPARE(h.revision("ig"), 0U);
    h.add("ig", 1.0, kTorr, at(0));
    const unsigned one = h.revision("ig");
    QVERIFY(one != 0U);
    h.add("ig", 1.0, kTorr, at(1));
    QVERIFY(h.revision("ig") != one);
  }

  // No assertion on the time: it says whether a day's chart needs thinning.
  void aDayOfSecondsIsReadQuickly() {
    ReadingHistory h({"ig"}, 3s);
    for (int i = 0; i < 86400; ++i) h.add("ig", 1e-8 + i, kTorr, at(i));
    QCOMPARE(h.series("ig", kDay).points.size(), std::size_t{86400});
    QCOMPARE(h.series("ig", 10min).points.size(), std::size_t{601});
    QElapsedTimer timer;
    timer.start();
    std::size_t n = 0;
    for (int i = 0; i < 1000; ++i) n += h.series("ig", 10min).points.size();
    const qint64 ten_minutes_us = timer.nsecsElapsed() / 1000;
    timer.restart();
    for (int i = 0; i < 1000; ++i) n += h.series("ig", kDay).points.size();
    const qint64 day_us = timer.nsecsElapsed() / 1000;
    QVERIFY(n > 0);
    qInfo("series(10 min): %lld us a call; series(24 h): %lld us a call", static_cast<long long>(ten_minutes_us / 1000),
          static_cast<long long>(day_us / 1000));
  }
};

QTEST_APPLESS_MAIN(TestReadingHistory)
#include "test_reading_history.moc"
