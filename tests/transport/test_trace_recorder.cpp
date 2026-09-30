#include "pychron/transport/trace_recorder.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <sstream>

#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions opts() {
  TransportOptions o;
  o.name = "rec";
  o.timeout = 50ms;
  return o;
}

}  // namespace

TEST(TraceRecorder, RecordsTxRxAndErrorsWithTimestamps) {
  ManualClock clock;
  auto sim = SimTransport::scripted({{to_bytes("PR1\r\n"), to_bytes("0,1E-8\r\n")},
                                     {to_bytes("PR2\r\n"), {}},
                                     {to_bytes("SET\r\n"), {}}},
                                    opts());
  auto sink = std::make_shared<std::ostringstream>();
  TraceRecorder rec(std::move(sim), sink, clock);
  EXPECT_EQ(rec.name(), "rec");
  ASSERT_TRUE(rec.open());

  clock.advance(1ms);
  ASSERT_TRUE(rec.exchange(to_bytes("PR1\r\n"), ReadSpec::until("\r\n")));
  clock.advance(1ms);
  EXPECT_FALSE(rec.exchange(to_bytes("PR2\r\n"), ReadSpec::until("\r\n")));
  EXPECT_EQ(rec.health().state, HealthState::Degraded);
  ASSERT_TRUE(rec.write(to_bytes("SET\r\n")));

  std::istringstream in(sink->str());
  auto records = parse_trace(in);
  ASSERT_TRUE(records) << to_string(records.error());
  ASSERT_EQ(records->size(), 5u);
  EXPECT_EQ((*records)[0], (TraceRecord{1000us, TraceRecord::Dir::Tx, to_bytes("PR1\r\n"), {}}));
  EXPECT_EQ((*records)[1], (TraceRecord{1000us, TraceRecord::Dir::Rx, to_bytes("0,1E-8\r\n"), {}}));
  EXPECT_EQ((*records)[2].at, 2000us);
  EXPECT_EQ((*records)[3].dir, TraceRecord::Dir::Err);
  EXPECT_EQ((*records)[3].message.rfind("timeout", 0), 0u);
  EXPECT_EQ((*records)[4].data, to_bytes("SET\r\n"));
}

TEST(TraceRecorder, RecordedTraceReplaysIdentically) {
  ManualClock clock;
  auto live = SimTransport::hooked([](const Bytes& tx) { return to_bytes("re:" + to_string(tx)); }, opts());
  auto sink = std::make_shared<std::ostringstream>();
  {
    TraceRecorder rec(std::move(live), sink, clock);
    ASSERT_TRUE(rec.open());
    ASSERT_TRUE(rec.exchange(to_bytes("a\n"), ReadSpec::until("\n")));
    ASSERT_TRUE(rec.exchange(to_bytes("b\n"), ReadSpec::until("\n")));
  }
  std::istringstream in(sink->str());
  auto replay = SimTransport::replay(*parse_trace(in), opts());
  ASSERT_TRUE(replay->open());
  EXPECT_EQ(to_string(*replay->exchange(to_bytes("a\n"), ReadSpec::until("\n"))), "re:a\n");
  EXPECT_EQ(to_string(*replay->exchange(to_bytes("b\n"), ReadSpec::until("\n"))), "re:b\n");
  EXPECT_TRUE(replay->verify());
}

TEST(TraceRecorder, ReadIsRecordedAsRx) {
  ManualClock clock;
  auto sim = SimTransport::scripted({{to_bytes("go"), to_bytes("x;y;")}}, opts());
  auto sink = std::make_shared<std::ostringstream>();
  TraceRecorder rec(std::move(sim), sink, clock);
  ASSERT_TRUE(rec.open());
  ASSERT_TRUE(rec.write(to_bytes("go")));
  ASSERT_TRUE(rec.read(ReadSpec::until(";")));
  EXPECT_NE(sink->str().find("rx 783b"), std::string::npos);
  rec.close();
  EXPECT_EQ(rec.health().state, HealthState::Down);
}

TEST(TraceRecorder, ToFileWritesReplayableTrace) {
  const auto path = std::filesystem::temp_directory_path() / "pychron_trace_recorder_test.txt";
  ManualClock clock;
  {
    auto rec = TraceRecorder::to_file(
        SimTransport::hooked([](const Bytes&) { return to_bytes("ok\n"); }, opts()), path.string(), clock);
    ASSERT_TRUE(rec);
    ASSERT_TRUE((*rec)->open());
    ASSERT_TRUE((*rec)->exchange(to_bytes("q\n"), ReadSpec::until("\n")));
  }
  auto records = load_trace(path.string());
  ASSERT_TRUE(records);
  EXPECT_EQ(records->size(), 2u);
  std::filesystem::remove(path);
}

TEST(TraceRecorder, ToFileUnwritablePathIsIoError) {
  ManualClock clock;
  auto rec = TraceRecorder::to_file(SimTransport::scripted({}, opts()), "/nonexistent/dir/trace.txt", clock);
  ASSERT_FALSE(rec);
  EXPECT_EQ(rec.error().kind, ErrorKind::Io);
}
