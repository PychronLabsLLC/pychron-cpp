#include "pychron/devices/device.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "log_capture.hpp"
#include "pychron/core/log_context.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

class Plain : public Device {
 public:
  using Device::Device;
  using Device::begin;
  using Device::call;
  using Device::observe;

  // One operation, observed without a name of its own in the test.
  template <class T>
  Result<T> observe(Result<T> result) {
    return observe(begin("op"), std::move(result));
  }
};

bool has_line(const std::vector<std::string>& lines, const std::string& wanted) {
  return std::ranges::find(lines, wanted) != lines.end();
}

}  // namespace

TEST(Device, HasNameAndStartsUnknown) {
  Plain d("ig1");
  EXPECT_EQ(d.name(), "ig1");
  DeviceHealth h = d.health();
  EXPECT_EQ(h.state, DeviceState::Unknown);
  EXPECT_EQ(h.consecutive_failures, 0u);
  EXPECT_FALSE(h.last_error.has_value());
  EXPECT_EQ(h.last_ok, TimePoint{});
}

TEST(Device, SuccessStampsLastOkFromClock) {
  ManualClock clock(TimePoint{} + 5s);
  Plain d("ig1", DeviceOptions{&clock, 3});
  Result<double> r = d.observe(Result<double>(1.5));
  ASSERT_TRUE(r);
  EXPECT_DOUBLE_EQ(*r, 1.5);
  DeviceHealth h = d.health();
  EXPECT_EQ(h.state, DeviceState::Ok);
  EXPECT_EQ(h.last_ok, TimePoint{} + 5s);
}

TEST(Device, FailuresDegradeThenFault) {
  ManualClock clock;
  Plain d("ig1", DeviceOptions{&clock, 2});
  (void)d.observe(Result<void>{});
  (void)d.observe(Result<void>(fail(ErrorKind::Timeout, "no reply")));
  EXPECT_EQ(d.health().state, DeviceState::Degraded);
  EXPECT_EQ(d.health().consecutive_failures, 1u);
  EXPECT_EQ(d.health().last_error, ErrorKind::Timeout);

  (void)d.observe(Result<void>(fail(ErrorKind::Protocol, "garbled")));
  EXPECT_EQ(d.health().state, DeviceState::Faulted);
  EXPECT_EQ(d.health().consecutive_failures, 2u);
  EXPECT_EQ(d.health().last_error, ErrorKind::Protocol);
}

TEST(Device, SuccessResetsFailureCountButKeepsLastError) {
  Plain d("ig1", DeviceOptions{nullptr, 1});
  (void)d.observe(Result<int>(fail(ErrorKind::Io, "unplugged")));
  EXPECT_EQ(d.health().state, DeviceState::Faulted);
  (void)d.observe(Result<int>(3));
  DeviceHealth h = d.health();
  EXPECT_EQ(h.state, DeviceState::Ok);
  EXPECT_EQ(h.consecutive_failures, 0u);
  EXPECT_EQ(h.last_error, ErrorKind::Io);
  EXPECT_NE(h.last_ok, TimePoint{});  // SteadyClock used when none is injected
}

TEST(Device, ObserveAttributesUnnamedErrorsToDevice) {
  Plain d("ig1");
  Result<int> r = d.observe(Result<int>(fail(ErrorKind::NotConnected, "port closed")));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().device, "ig1");
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);

  Result<void> named = d.observe(Result<void>(fail(ErrorKind::Io, "x", "valve_bus")));
  EXPECT_EQ(named.error().device, "valve_bus");
}

TEST(Device, CancelledDoesNotCountAsDeviceFailure) {
  Plain d("ig1");
  (void)d.observe(Result<void>{});
  (void)d.observe(Result<void>(fail(ErrorKind::Cancelled, "shutting down")));
  EXPECT_EQ(d.health().state, DeviceState::Ok);
  EXPECT_EQ(d.health().consecutive_failures, 0u);
}

TEST(Device, StateNames) {
  EXPECT_EQ(to_string(DeviceState::Unknown), "unknown");
  EXPECT_EQ(to_string(DeviceState::Ok), "ok");
  EXPECT_EQ(to_string(DeviceState::Degraded), "degraded");
  EXPECT_EQ(to_string(DeviceState::Faulted), "faulted");
}

// ---- the log ---------------------------------------------------------------

TEST(DeviceLog, CallOkIsDebug) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  ManualClock clock;
  Plain d("ig1", DeviceOptions{&clock, 3});
  auto r = d.call<double>(
      "read_pressure",
      [&](Resend) -> Result<double> {
        clock.advance(std::chrono::milliseconds(12));
        return 1.5;
      },
      Repeat::Safe);
  ASSERT_TRUE(r);
  const auto lines = log.lines();
  EXPECT_TRUE(has_line(lines, "[debug] device.ig1: read_pressure ok in 12 ms")) << ::testing::PrintToString(lines);
}

TEST(DeviceLog, FailureIsWarn) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  log.hub().set_levels(LogLevel::Warn, {});
  Plain d("ig1");
  auto r = d.call<double>(
      "read_pressure", [](Resend) -> Result<double> { return fail(ErrorKind::Timeout, "no reply"); },
      Repeat::Safe);
  ASSERT_FALSE(r);
  const auto lines = log.lines();
  EXPECT_TRUE(has_line(lines, "[warn] device.ig1: read_pressure failed: timeout: no reply"))
      << ::testing::PrintToString(lines);
}

TEST(DeviceLog, PollingIsTrace) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  Plain d("ig1");
  {
    const LogContext::Polling polling;
    auto r = d.call<double>("read_pressure", [](Resend) -> Result<double> { return 1.5; }, Repeat::Safe);
    ASSERT_TRUE(r);
  }
  for (const auto& line : log.lines()) EXPECT_EQ(line.find("device.ig1"), std::string::npos) << line;
}
