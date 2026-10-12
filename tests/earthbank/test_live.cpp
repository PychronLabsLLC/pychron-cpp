// The live test's steps (live_round_trip.hpp), run against the fake: the
// package made, both fixture samples uploaded, nothing sent twice, and the
// package gone at the end, whether the steps passed or not.

#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "fake_earthbank.hpp"
#include "live_round_trip.hpp"
#include "pychron/earthbank/session.hpp"

namespace pychron::earthbank::testing {
namespace {

RoundTrip against(const FakeEarthBank& bank, std::ostream& out) {
  RoundTrip with;
  with.base_url = bank.base_url();
  with.username = bank.login();
  with.password = bank.password();
  with.institution_id = 9301;
  with.out = &out;
  return with;
}

TEST(LiveRoundTrip, UploadsBothSamplesOnceAndTakesItsPackageAway) {
  FakeEarthBank bank;
  const std::size_t packages_before = bank.store().packages.size();
  std::ostringstream out;
  run_round_trip(bank, against(bank, out));
  if (HasFatalFailure()) return;

  const std::string said = out.str();
  EXPECT_NE(said.find("made data package 620001 'pychron-test-"), std::string::npos) << said;
  EXPECT_NE(said.find("S-FUS: complete, data point "), std::string::npos) << said;
  EXPECT_NE(said.find("S-STEP: complete, data point "), std::string::npos) << said;
  EXPECT_NE(said.find("a second upload sent nothing new"), std::string::npos) << said;
  EXPECT_NE(said.find("removed data package 620001"), std::string::npos) << said;
  // Each record went once: 2 data points, 7 aliquots, 16 measurements.
  EXPECT_EQ(bank.count("POST", "/api/arar/ArArDataPoint"), 2u);
  EXPECT_EQ(bank.count("POST", "/api/arar/ArArAliquot"), 7u);
  EXPECT_EQ(bank.count("POST", "/api/arar/ArArMeasurement"), 16u);
  // And nothing of it is left on the service.
  const Store left = bank.store();
  EXPECT_EQ(left.packages.size(), packages_before);
  EXPECT_TRUE(left.samples.empty());
  EXPECT_TRUE(left.data_points.empty());
  EXPECT_TRUE(left.measurements.empty());
  EXPECT_EQ(said.find(bank.password()), std::string::npos);
}

TEST(LiveRoundTrip, ThePackageIsTakenAwayWhenTheUploadFails) {
  FakeEarthBank bank;
  const std::size_t packages_before = bank.store().packages.size();
  bank.fail_next("POST", "/api/arar/ArArDataPoint", 400, R"({"detail":"no"})");
  std::ostringstream out;
  // The steps fail (a data point was refused); what matters here is after.
  {
    ::testing::TestPartResultArray failures;
    {
      const ::testing::ScopedFakeTestPartResultReporter reporter(
          ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
      run_round_trip(bank, against(bank, out));
    }
    EXPECT_GE(failures.size(), 1);
  }
  EXPECT_NE(out.str().find("removed data package 620001"), std::string::npos) << out.str();
  EXPECT_EQ(bank.store().packages.size(), packages_before);
  EXPECT_TRUE(bank.store().samples.empty());
}

TEST(LiveRoundTrip, APackageThatCannotBeRemovedIsAFailureThatNamesIt) {
  FakeEarthBank bank;
  bank.fail_next("DELETE", "/api/management/data-packages/", 500, R"({"detail":"no"})");
  std::ostringstream out;
  ::testing::TestPartResultArray failures;
  {
    const ::testing::ScopedFakeTestPartResultReporter reporter(
        ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    run_round_trip(bank, against(bank, out));
  }
  ASSERT_EQ(failures.size(), 1);
  EXPECT_NE(std::string(failures.GetTestPartResult(0).message()).find("620001"), std::string::npos);
  EXPECT_NE(out.str().find("was NOT removed"), std::string::npos) << out.str();
  EXPECT_NE(out.str().find("remove it on EarthBank by hand"), std::string::npos);
}

TEST(LiveRoundTrip, ThePackageIsNamedForTheTimeInUtc) {
  const auto t = std::chrono::system_clock::from_time_t(1949755353);  // 2031-10-14 14:42:33 UTC
  EXPECT_EQ(test_package_name(t), "pychron-test-20311014-144233");
}

}  // namespace
}  // namespace pychron::earthbank::testing
