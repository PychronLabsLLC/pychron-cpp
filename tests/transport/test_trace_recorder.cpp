#include "pychron/transport/trace_recorder.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/signal_bus.hpp"
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

namespace {

struct WireCapture {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  SignalBus::Subscription sub;
  std::shared_ptr<LogHub> hub;

  explicit WireCapture(LogLevel level = LogLevel::Trace) {
    sub = bus.subscribe<Log>([this](const Log& e) { got.push_back(e); });
    config::LoggingConfig cfg;
    cfg.default_level = level;
    auto h = LogHub::create(cfg, clock, &bus);
    hub = *h;
  }
};

}  // namespace

TEST(TraceRecorder, MirrorsBytesToWireLogger) {
  WireCapture cap;
  auto sim = SimTransport::scripted({{to_bytes("PR1\r\n"), to_bytes("0,1E-8\r\n")}}, opts());
  auto sink = std::make_shared<std::ostringstream>();
  TraceRecorder rec(std::move(sim), sink, cap.clock, cap.hub->logger("rec.wire"));
  ASSERT_TRUE(rec.open());
  ASSERT_TRUE(rec.exchange(to_bytes("PR1\r\n"), ReadSpec::until("\r\n")));

  ASSERT_EQ(cap.got.size(), 2u);
  EXPECT_EQ(cap.got[0].logger, "rec.wire");
  EXPECT_EQ(cap.got[0].level, LogLevel::Trace);
  EXPECT_EQ(cap.got[0].message, "tx 5B 50 52 31 0D 0A |PR1..|");
  EXPECT_EQ(cap.got[1].message, "rx 8B 30 2C 31 45 2D 38 0D 0A |0,1E-8..|");
}

TEST(TraceRecorder, WireErrorRecordsFollowTraceErrors) {
  WireCapture cap;
  auto sim = SimTransport::scripted({{to_bytes("PR2\r\n"), {}}}, opts());
  auto sink = std::make_shared<std::ostringstream>();
  TraceRecorder rec(std::move(sim), sink, cap.clock, cap.hub->logger("rec.wire"));
  ASSERT_TRUE(rec.open());
  EXPECT_FALSE(rec.exchange(to_bytes("PR2\r\n"), ReadSpec::until("\r\n")));

  ASSERT_EQ(cap.got.size(), 2u);
  EXPECT_EQ(cap.got[0].message, "tx 5B 50 52 32 0D 0A |PR2..|");
  EXPECT_EQ(cap.got[1].message.rfind("err timeout", 0), 0u);
}

TEST(TraceRecorder, NoLoggerOutputIsByteIdentical) {
  ManualClock clock;
  auto run = [&](std::optional<Logger> wire) {
    auto sim = SimTransport::scripted({{to_bytes("PR1\r\n"), to_bytes("0,1E-8\r\n")}}, opts());
    auto sink = std::make_shared<std::ostringstream>();
    TraceRecorder rec(std::move(sim), sink, clock, std::move(wire));
    EXPECT_TRUE(rec.open());
    EXPECT_TRUE(rec.exchange(to_bytes("PR1\r\n"), ReadSpec::until("\r\n")));
    return sink->str();
  };
  WireCapture cap;
  const std::string plain = run(std::nullopt);
  EXPECT_EQ(plain, "# trace of transport 'rec'\n0 tx 5052310d0a\n0 rx 302c31452d380d0a\n");
  EXPECT_EQ(run(cap.hub->logger("rec.wire")), plain);
}

TEST(TraceRecorder, WireDisabledBelowTraceLevel) {
  WireCapture cap(LogLevel::Info);
  auto sim = SimTransport::scripted({{to_bytes("a"), to_bytes("b")}}, opts());
  auto sink = std::make_shared<std::ostringstream>();
  TraceRecorder rec(std::move(sim), sink, cap.clock, cap.hub->logger("rec.wire"));
  ASSERT_TRUE(rec.open());
  ASSERT_TRUE(rec.exchange(to_bytes("a"), ReadSpec::fixed(1)));
  EXPECT_TRUE(cap.got.empty());
}

TEST(TraceRecorder, WireOrderMatchesTraceOrder) {
  WireCapture cap;
  auto live = SimTransport::hooked([](const Bytes& tx) { return tx; }, opts());
  auto sink = std::make_shared<std::ostringstream>();
  std::mutex got_mutex;
  std::vector<std::string> wire;
  auto sub2 = cap.bus.subscribe<Log>([&](const Log& e) {
    std::lock_guard l(got_mutex);
    wire.push_back(e.message);
  });
  TraceRecorder rec(std::move(live), sink, cap.clock, cap.hub->logger("rec.wire"));
  ASSERT_TRUE(rec.open());
  auto work = [&](char c) {
    for (int i = 0; i < 50; ++i) (void)rec.exchange(to_bytes(std::string(1, c) + "\n"), ReadSpec::until("\n"));
  };
  std::thread a(work, 'a'), b(work, 'b');
  a.join();
  b.join();

  std::istringstream in(sink->str());
  auto records = parse_trace(in);
  ASSERT_TRUE(records);
  ASSERT_EQ(records->size(), 200u);
  ASSERT_EQ(wire.size(), 200u);
  for (std::size_t i = 0; i < wire.size(); ++i) {
    const auto& r = (*records)[i];
    const char* dir = r.dir == TraceRecord::Dir::Tx ? "tx" : "rx";
    EXPECT_EQ(wire[i].rfind(dir, 0), 0u) << i;
    const char ch = static_cast<char>(r.data[0]);
    EXPECT_NE(wire[i].find(std::string("|") + ch + ".|"), std::string::npos) << i;
  }
}
