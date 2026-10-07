#include "pychron/transport/sim_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include "pychron/core/virtual_clock.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions opts(int retries = 0) {
  TransportOptions o;
  o.name = "sim1";
  o.timeout = 100ms;
  o.retries = retries;
  return o;
}

const ReadSpec kCrLf = ReadSpec::until("\r\n");

std::unique_ptr<SimTransport> open_scripted(std::vector<SimStep> steps, int retries = 0) {
  auto t = SimTransport::scripted(std::move(steps), opts(retries));
  EXPECT_TRUE(t->open());
  return t;
}

// A read that waits past the clock leaves time standing and the test asleep
// in it. Each test on a VirtualClock runs under a real-time bound: when it is
// exceeded the process says so and aborts, well inside the ctest timeout.
class SimTransportVirtual : public pychron::testing::VirtualTimeTest {};

}  // namespace

TEST(SimTransportScripted, RepliesInOrder) {
  auto t = open_scripted({{to_bytes("PR1\r\n"), to_bytes("0, 1.0E-08\r\n")},
                          {to_bytes("PR2\r\n"), to_bytes("0, 2.0E-08\r\n")}});
  EXPECT_EQ(to_string(*t->exchange(to_bytes("PR1\r\n"), kCrLf)), "0, 1.0E-08\r\n");
  EXPECT_EQ(to_string(*t->exchange(to_bytes("PR2\r\n"), kCrLf)), "0, 2.0E-08\r\n");
  EXPECT_TRUE(t->verify());
  EXPECT_EQ(t->written().size(), 2u);
}

TEST(SimTransportScripted, UnexpectedTxFailsAndIsReported) {
  auto t = open_scripted({{to_bytes("PR1\r\n"), to_bytes("ok\r\n")}});
  auto r = t->exchange(to_bytes("PR9\r\n"), kCrLf);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  auto v = t->verify();
  ASSERT_FALSE(v);
  EXPECT_NE(v.error().what.find("PR9"), std::string::npos);
}

TEST(SimTransportScripted, TxAfterScriptEndIsUnexpected) {
  auto t = open_scripted({});
  EXPECT_FALSE(t->write(to_bytes("X")));
  EXPECT_FALSE(t->verify());
}

TEST(SimTransportScripted, VerifyReportsUnconsumedSteps) {
  auto t = open_scripted({{to_bytes("A"), {}}, {to_bytes("B"), {}}});
  ASSERT_TRUE(t->write(to_bytes("A")));
  auto v = t->verify();
  ASSERT_FALSE(v);
  EXPECT_NE(v.error().what.find("1 step"), std::string::npos);
  t->expect({to_bytes("C"), {}});
  ASSERT_TRUE(t->write(to_bytes("B")));
  ASSERT_TRUE(t->write(to_bytes("C")));
  EXPECT_TRUE(t->verify());
}

TEST(SimTransportScripted, StepDelayBeyondTimeoutTimesOut) {
  auto t = open_scripted({{to_bytes("Q"), to_bytes("slow\r\n"), 150ms}});
  auto r = t->exchange(to_bytes("Q"), kCrLf);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
}

TEST(SimTransportScripted, StepDelayWithinTimeoutSucceeds) {
  auto t = open_scripted({{to_bytes("Q"), to_bytes("ok\r\n"), 150ms}});
  EXPECT_TRUE(t->exchange(to_bytes("Q"), kCrLf, 200ms));
}

TEST(SimTransportScripted, LateReplyIsDiscardedBeforeNextExchange) {
  auto t = open_scripted({{to_bytes("A"), to_bytes("late\r\n"), 1s},
                          {to_bytes("B"), to_bytes("fresh\r\n")}});
  EXPECT_FALSE(t->exchange(to_bytes("A"), kCrLf));
  EXPECT_EQ(to_string(*t->exchange(to_bytes("B"), kCrLf)), "fresh\r\n");
}

TEST(SimTransportScripted, WriteThenReadSplitsExchange) {
  auto t = open_scripted({{to_bytes("A"), to_bytes("one\r\ntwo\r\n")}});
  ASSERT_TRUE(t->write(to_bytes("A")));
  EXPECT_EQ(to_string(*t->read(kCrLf)), "one\r\n");
  EXPECT_EQ(to_string(*t->read(kCrLf)), "two\r\n");
  EXPECT_EQ(t->read(kCrLf).error().kind, ErrorKind::Timeout);
}

TEST(SimTransportScripted, IncompleteFrameTimesOut) {
  auto t = open_scripted({{to_bytes("A"), to_bytes("no terminator")}});
  EXPECT_EQ(t->exchange(to_bytes("A"), kCrLf).error().kind, ErrorKind::Timeout);
}

TEST(SimTransportFaults, DropNextCausesTimeoutThenRetrySucceeds) {
  auto t = open_scripted({{to_bytes("A"), to_bytes("r1\r\n")}, {to_bytes("A"), to_bytes("r2\r\n")}},
                         /*retries=*/1);
  t->drop_next();
  auto r = t->exchange(to_bytes("A"), kCrLf);
  ASSERT_TRUE(r);
  EXPECT_EQ(to_string(*r), "r2\r\n");
  EXPECT_TRUE(t->verify());
}

TEST(SimTransportFaults, DropNextCountsReplies) {
  auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("ok\r\n"); }, opts());
  ASSERT_TRUE(t->open());
  t->drop_next(2);
  EXPECT_FALSE(t->exchange(to_bytes("a"), kCrLf));
  EXPECT_FALSE(t->exchange(to_bytes("b"), kCrLf));
  EXPECT_TRUE(t->exchange(to_bytes("c"), kCrLf));
}

TEST(SimTransportFaults, DelayNextAppliesOnce) {
  auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("ok\r\n"); }, opts());
  ASSERT_TRUE(t->open());
  t->delay_next(500ms);
  EXPECT_EQ(t->exchange(to_bytes("a"), kCrLf).error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(t->exchange(to_bytes("a"), kCrLf));
}

TEST(SimTransportFaults, GarbleNextKeepsFramingButCorruptsPayload) {
  auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("ok\r\n"); }, opts());
  ASSERT_TRUE(t->open());
  t->garble_next();
  auto r = t->exchange(to_bytes("a"), kCrLf);
  ASSERT_TRUE(r);
  EXPECT_NE(to_string(*r), "ok\r\n");
  EXPECT_EQ(r->size(), 4u);
  EXPECT_EQ(to_string(*t->exchange(to_bytes("a"), kCrLf)), "ok\r\n");
}

TEST(SimTransportFaults, GarbledAnyOfFrameKeepsDelimiters) {
  auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("\nok\r"); }, opts());
  ASSERT_TRUE(t->open());
  t->garble_next();
  auto r = t->exchange(to_bytes("a"), ReadSpec::until_any("\r\n"));
  ASSERT_TRUE(r);
  ASSERT_EQ(r->size(), 4u);
  EXPECT_EQ((*r)[0], '\n');
  EXPECT_NE((*r)[1], 'o');
  EXPECT_EQ((*r)[3], '\r');
}

TEST(SimTransportFaults, GarbledModbusFrameKeepsLengthButBreaksCrc) {
  const Bytes reply{0x01, 0x03, 0x02, 0x00, 0x07, 0xF9, 0x86};
  auto t = SimTransport::hooked([reply](const Bytes&) { return reply; }, opts());
  ASSERT_TRUE(t->open());
  t->garble_next();
  auto r = t->exchange(Bytes{0x01, 0x03, 0, 0, 0, 1, 0x84, 0x0A}, ReadSpec::modbus_rtu());
  ASSERT_TRUE(r);
  ASSERT_EQ(r->size(), reply.size());
  EXPECT_EQ(Bytes(r->begin(), r->begin() + 5), Bytes(reply.begin(), reply.begin() + 5));
  EXPECT_NE(*r, reply);
}

TEST(SimTransportFaults, FailOpenNext) {
  auto t = SimTransport::scripted({}, opts());
  t->fail_open_next();
  auto r = t->open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_TRUE(t->open());
}

TEST(SimTransportHook, MapsTxToReply) {
  int calls = 0;
  auto t = SimTransport::hooked(
      [&](const Bytes& tx) {
        ++calls;
        return to_bytes("echo:" + to_string(tx));
      },
      opts());
  ASSERT_TRUE(t->open());
  EXPECT_EQ(to_string(*t->exchange(to_bytes("hi\r\n"), kCrLf)), "echo:hi\r\n");
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(t->verify());
}

TEST(SimTransportHook, EmptyReplyMeansNoReply) {
  auto t = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, opts());
  ASSERT_TRUE(t->open());
  EXPECT_TRUE(t->write(to_bytes("set")));
  EXPECT_EQ(t->exchange(to_bytes("get"), kCrLf).error().kind, ErrorKind::Timeout);
}

TEST(SimTransportReplay, BuildsStepsFromTraceRecords) {
  std::vector<TraceRecord> trace{
      {0us, TraceRecord::Dir::Tx, to_bytes("PR1\r\n"), {}},
      {900us, TraceRecord::Dir::Rx, to_bytes("0, 1.0E-08\r\n"), {}},
      {2000us, TraceRecord::Dir::Tx, to_bytes("PR2\r\n"), {}},
      {2100us, TraceRecord::Dir::Err, {}, "timeout no reply"},
      {3000us, TraceRecord::Dir::Tx, to_bytes("SET\r\n"), {}},
  };
  auto t = SimTransport::replay(trace, opts());
  ASSERT_TRUE(t->open());
  EXPECT_EQ(to_string(*t->exchange(to_bytes("PR1\r\n"), kCrLf)), "0, 1.0E-08\r\n");
  EXPECT_EQ(t->exchange(to_bytes("PR2\r\n"), kCrLf).error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(t->write(to_bytes("SET\r\n")));
  EXPECT_TRUE(t->verify());
}

TEST(SimTransportReplay, MissingFileIsConfigError) {
  auto r = SimTransport::replay(std::string("/nonexistent/trace.txt"), opts());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

// An instrument's event stream: input nobody wrote for, interleaved with
// replies in arrival order, framed like any other input.
TEST(SimTransportHooked, UnsolicitedInputIsReadAndWaitedFor) {
  std::mutex m;
  std::string due;  // what the source hands over next
  auto source = [&] {
    std::lock_guard lock(m);
    return to_bytes(std::exchange(due, std::string{}));
  };
  auto t = SimTransport::hooked([](const Bytes&) { return to_bytes(std::string("OK\r\n")); }, opts(), source);
  ASSERT_TRUE(t->open());
  {
    std::lock_guard lock(m);
    due = "EVENT 1\r\n";
  }
  ASSERT_TRUE(t->write(to_bytes(std::string("Q\r"))));
  // The event was due before the command: it is read first.
  EXPECT_EQ(*t->read(kCrLf), to_bytes(std::string("EVENT 1\r\n")));
  EXPECT_EQ(*t->read(kCrLf), to_bytes(std::string("OK\r\n")));
  // A read with nothing complete waits (real time) for the source.
  std::thread late([&] {
    std::this_thread::sleep_for(20ms);
    std::lock_guard lock(m);
    due = "EVENT 2\r\n";
  });
  const auto t0 = std::chrono::steady_clock::now();
  auto second = t->read(kCrLf, 2s);
  late.join();
  ASSERT_TRUE(second) << second.error().what;
  EXPECT_EQ(*second, to_bytes(std::string("EVENT 2\r\n")));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  // Nothing due: a Timeout after (about) the timeout, not at once.
  const auto t1 = std::chrono::steady_clock::now();
  auto none = t->read(kCrLf, 30ms);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Timeout);
  EXPECT_GE(std::chrono::steady_clock::now() - t1, 25ms);
}

// poll(): nothing yet is not a failure and leaves health alone; a frame is
// returned like read() returns it.
TEST(SimTransportHooked, PollTreatsNothingYetAsNormal) {
  std::mutex m;
  std::string due;
  auto t = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, opts(), [&] {
    std::lock_guard lock(m);
    return to_bytes(std::exchange(due, std::string{}));
  });
  ASSERT_TRUE(t->open());
  for (int i = 0; i < 5; ++i) {
    auto r = t->poll(kCrLf, 5ms);
    ASSERT_TRUE(r) << r.error().what;
    EXPECT_FALSE(*r);
  }
  EXPECT_EQ(t->health().consecutive_failures, 0u);
  {
    std::lock_guard lock(m);
    due = "EVENT\r\n";
  }
  auto r = t->poll(kCrLf, 1s);
  ASSERT_TRUE(r && *r);
  EXPECT_EQ(**r, to_bytes(std::string("EVENT\r\n")));
  EXPECT_EQ(t->health().state, HealthState::Connected);
  t->close();
  auto closed = t->poll(kCrLf, 5ms);
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error().kind, ErrorKind::NotConnected);
}

TEST(SimTransportHooked, UntilCloseReturnsTheWholeReply) {
  // A simulated peer has said everything once its reply is queued.
  auto t = SimTransport::hooked([](const Bytes&) { return to_bytes("OK"); });
  ASSERT_TRUE(t->open());
  auto r = t->exchange(to_bytes("Open A\r"), ReadSpec::until_close());
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(to_string(*r), "OK");
  auto silent = SimTransport::hooked([](const Bytes&) { return Bytes{}; });
  ASSERT_TRUE(silent->open());
  auto none = silent->exchange(to_bytes("Open A\r"), ReadSpec::until_close(), std::chrono::milliseconds(10));
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Timeout);
}

TEST_F(SimTransportVirtual, UnsolicitedReadTimesOutInClockTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  auto o = opts();
  o.clock = &clock;
  auto t = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, o, [] { return Bytes{}; });
  ASSERT_TRUE(t->open());

  const auto real_start = std::chrono::steady_clock::now();
  auto r = t->read(kCrLf, 3s);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(clock.now(), kStart + 3s);
  // Three thousand one-millisecond polls of the clock, each a hand-over between
  // threads: no real time to speak of, but not nothing on a loaded machine.
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 20s);
}

TEST_F(SimTransportVirtual, UnsolicitedReadSeesLateInput) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  auto o = opts();
  o.clock = &clock;
  bool sent = false;  // worker thread only
  auto t = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, o, [&] {
    if (sent || clock.now() < kStart + 1s) return Bytes{};
    sent = true;
    return to_bytes(std::string("EVENT\r\n"));
  });
  ASSERT_TRUE(t->open());

  auto r = t->read(kCrLf, 3s);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(*r, to_bytes(std::string("EVENT\r\n")));
  EXPECT_GE(clock.now(), kStart + 1s);
  EXPECT_LE(clock.now(), kStart + 1s + 2ms);
}
