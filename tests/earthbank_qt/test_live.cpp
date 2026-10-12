// The live test: one upload to a real EarthBank, by a person, on purpose.
//
// It does nothing unless all four of these are set:
//
//   PYCHRON_TEST_EARTHBANK_URL          https://... (the service, or a staging copy of it)
//   PYCHRON_TEST_EARTHBANK_USER         the login
//   PYCHRON_TEST_EARTHBANK_PASSWORD     its password
//   PYCHRON_TEST_EARTHBANK_INSTITUTION  the institution's id on EarthBank
//
// No build, script or CI job sets them; without them the test is skipped and
// says so. It carries the ctest label `live`, so `ctest -LE live` leaves it
// out altogether. With them it writes to that service: a data package named
// `pychron-test-<UTC date-time>`, the two fixture samples in it, and then the
// package's removal. The steps, and what to do when one fails, are in
// docs/earthbank.md ("The live test"); they are tests/earthbank's
// live_round_trip.hpp, which every build runs against the fake.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

#include "live_round_trip.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/qt_http_client.hpp"

namespace pychron::earthbank::testing {
namespace {

std::string env(const char* name) {
  const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once, before any thread is made
  return value != nullptr ? std::string(value) : std::string();
}

TEST(EarthbankLive, UploadsTheFixtureSamplesAndRemovesThem) {
  RoundTrip with;
  with.base_url = env("PYCHRON_TEST_EARTHBANK_URL");
  with.username = env("PYCHRON_TEST_EARTHBANK_USER");
  with.password = env("PYCHRON_TEST_EARTHBANK_PASSWORD");
  const std::string institution = env("PYCHRON_TEST_EARTHBANK_INSTITUTION");
  if (with.base_url.empty() || with.username.empty() || with.password.empty() || institution.empty()) {
    GTEST_SKIP() << "not run: set PYCHRON_TEST_EARTHBANK_URL, _USER, _PASSWORD and _INSTITUTION to upload to a real "
                    "EarthBank (docs/earthbank.md, \"The live test\")";
  }
  // The address is held to what a profile's is: https, or http on this machine.
  const auto url = checked_base_url(with.base_url);
  ASSERT_TRUE(url) << to_string(url.error());
  with.base_url = *url;
  char* end = nullptr;
  const long long id = std::strtoll(institution.c_str(), &end, 10);
  ASSERT_TRUE(end != nullptr && *end == '\0' && id > 0) << "PYCHRON_TEST_EARTHBANK_INSTITUTION is not a number";
  with.institution_id = static_cast<std::int64_t>(id);
  with.out = &std::cout;

  QtHttpClient http;
  run_round_trip(http, with);
}

}  // namespace
}  // namespace pychron::earthbank::testing
