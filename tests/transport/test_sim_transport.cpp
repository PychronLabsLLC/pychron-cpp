#include "pychron/transport/sim_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>

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
