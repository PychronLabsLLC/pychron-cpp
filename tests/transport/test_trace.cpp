#include "pychron/transport/trace.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace pychron;
using namespace std::chrono_literals;

TEST(Trace, FormatsRecords) {
  EXPECT_EQ(format_trace_record({1500us, TraceRecord::Dir::Tx, to_bytes("A\r"), {}}), "1500 tx 410d");
  EXPECT_EQ(format_trace_record({2us, TraceRecord::Dir::Rx, Bytes{0xff}, {}}), "2 rx ff");
  EXPECT_EQ(format_trace_record({3us, TraceRecord::Dir::Err, {}, "timeout no reply"}),
            "3 err timeout no reply");
}

TEST(Trace, ParseRoundTripsAndSkipsComments) {
  std::istringstream in(
      "# captured at the lab\n"
      "\n"
      "0 tx 5052310d0a\n"
      "850 rx 300d0a\n"
      "900 err timeout no reply\n"
      "1000 tx\n");
  auto r = parse_trace(in);
  ASSERT_TRUE(r) << to_string(r.error());
  ASSERT_EQ(r->size(), 4u);
  EXPECT_EQ((*r)[0], (TraceRecord{0us, TraceRecord::Dir::Tx, to_bytes("PR1\r\n"), {}}));
  EXPECT_EQ((*r)[1].at, 850us);
  EXPECT_EQ((*r)[1].dir, TraceRecord::Dir::Rx);
  EXPECT_EQ((*r)[2].message, "timeout no reply");
  EXPECT_TRUE((*r)[3].data.empty());
}

TEST(Trace, ParseRejectsMalformedLineWithLineNumber) {
  std::istringstream bad_dir("0 tx 41\n5 xx 41\n");
  auto r = parse_trace(bad_dir);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("line 2"), std::string::npos);

  std::istringstream bad_hex("0 rx 4\n");
  EXPECT_FALSE(parse_trace(bad_hex));
  std::istringstream bad_time("abc tx 41\n");
  EXPECT_FALSE(parse_trace(bad_time));
}

TEST(Trace, LoadMissingFileIsConfigError) {
  auto r = load_trace("/nonexistent/dir/trace.txt");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}
