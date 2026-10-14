// The peak center graph's scaling of an additional detector onto the
// reference's range (legacy's formula), for display only.

#include <algorithm>
#include <vector>

#include <QtTest/QtTest>

#include "peak_center_scale.hpp"

using pychron::ui::scale_to_reference;

namespace {

void compare_values(const std::vector<double>& actual, const std::vector<double>& expected) {
  QCOMPARE(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i)
    QVERIFY2(qFuzzyCompare(actual[i], expected[i]), qPrintable(QString::number(i)));
}

}  // namespace

class TestPeakCenterScale : public QObject {
  Q_OBJECT

 private slots:
  void mapsTheOthersRangeOntoTheReferences() {
    const std::vector<double> reference{10, 110, 60};
    const std::vector<double> other{1, 3, 2};
    const auto scaled = scale_to_reference(reference, other);
    QVERIFY(scaled.has_value());
    compare_values(*scaled, {10, 110, 60});
  }

  void handNumbers() {
    const std::vector<double> reference{2, 12};
    const std::vector<double> other{100, 150, 200};
    const auto scaled = scale_to_reference(reference, other);
    QVERIFY(scaled.has_value());
    compare_values(*scaled, {2, 7, 12});
  }

  void theScaledMaximumIsTheReferencesMaximum() {
    const std::vector<double> reference{0.13, 7.9, 3.3, 41.7, 12.2};
    const std::vector<double> other{1e-3, 4.2e-3, 9.87e-3, 2.5e-3};
    const auto scaled = scale_to_reference(reference, other);
    QVERIFY(scaled.has_value());
    QVERIFY(qFuzzyCompare(std::ranges::max(*scaled), std::ranges::max(reference)));
  }

  void theScaledMinimumIsTheReferencesMinimum() {
    const std::vector<double> reference{0.13, 7.9, 3.3, 41.7, 12.2};
    const std::vector<double> other{1e-3, 4.2e-3, 9.87e-3, 2.5e-3};
    const auto scaled = scale_to_reference(reference, other);
    QVERIFY(scaled.has_value());
    QVERIFY(qFuzzyCompare(std::ranges::min(*scaled), std::ranges::min(reference)));
  }

  void aFlatOtherIsNotScaled() {
    const std::vector<double> reference{1, 2, 3};
    const std::vector<double> other{5, 5, 5};
    QVERIFY(!scale_to_reference(reference, other).has_value());
  }

  void aFlatReferenceIsNotScaled() {
    const std::vector<double> reference{4, 4};
    const std::vector<double> other{1, 2, 3};
    QVERIFY(!scale_to_reference(reference, other).has_value());
  }

  void anEmptyInputIsNotScaled() {
    const std::vector<double> some{1, 2, 3};
    const std::vector<double> none;
    QVERIFY(!scale_to_reference(none, some).has_value());
    QVERIFY(!scale_to_reference(some, none).has_value());
  }

  void theInputsMayDifferInLength() {
    // An additional detector with a missing step has fewer values: each
    // range is taken over what each input has.
    const std::vector<double> reference{0, 5, 20, 10};
    const std::vector<double> other{2, 4, 3};
    const auto scaled = scale_to_reference(reference, other);
    QVERIFY(scaled.has_value());
    compare_values(*scaled, {0, 20, 10});
  }
};

QTEST_APPLESS_MAIN(TestPeakCenterScale)
#include "test_peak_center_scale.moc"
