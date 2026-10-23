// LogTeeBuf: what elctl writes to stderr still reaches stderr, byte for byte
// and at once, and each whole line of it is a record in the process log.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "log_capture.hpp"
#include "log_tee.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/log_hub.hpp"
#include "temp_path.hpp"
#include "pychron/core/process_log.hpp"

namespace elctl::testing {
namespace {

std::vector<std::string> tee_lines(const pychron::testing::LogCapture& capture) {
  std::vector<std::string> out;
  for (auto& line : capture.lines()) {
    if (line.find("] elctl: ") != std::string::npos) out.push_back(std::move(line));
  }
  return out;
}

TEST(LogTee, ErrorLineIsErrorRecord) {
  const pychron::testing::LogCapture capture;
  ASSERT_TRUE(capture.ok());
  std::ostringstream inner;
  {
    LogTeeBuf tee(inner.rdbuf(), pychron::process_logger("elctl"));
    std::ostream err(&tee);
    err << "error: no extraction line config at /x\n";
    err << "elctl: error: cannot open queue\n";
    err << "warning: 2 runs skipped\n";
    err << "elctl: virtual clock stalled\n";
  }
  EXPECT_EQ(tee_lines(capture), (std::vector<std::string>{
                                    "[error] elctl: error: no extraction line config at /x",
                                    "[error] elctl: elctl: error: cannot open queue",
                                    "[warn] elctl: warning: 2 runs skipped",
                                    "[warn] elctl: elctl: virtual clock stalled",
                                }));
}

TEST(LogTee, PartialLineWaitsForNewline) {
  const pychron::testing::LogCapture capture;
  ASSERT_TRUE(capture.ok());
  std::ostringstream inner;
  {
    LogTeeBuf tee(inner.rdbuf(), pychron::process_logger("elctl"));
    std::ostream err(&tee);
    err << "error: half";
    err.flush();
    EXPECT_TRUE(tee_lines(capture).empty());
    err << " and half\nleft over";
    EXPECT_EQ(tee_lines(capture), (std::vector<std::string>{"[error] elctl: error: half and half"}));
  }
  // What was never ended by a newline is logged when the tee goes.
  EXPECT_EQ(tee_lines(capture), (std::vector<std::string>{"[error] elctl: error: half and half",
                                                          "[warn] elctl: left over"}));
}

TEST(LogTee, PassesBytesThrough) {
  const pychron::testing::LogCapture capture;
  ASSERT_TRUE(capture.ok());
  std::ostringstream inner;
  LogTeeBuf tee(inner.rdbuf(), pychron::process_logger("elctl"));
  std::ostream err(&tee);
  err << "error: a";
  // Unbuffered: the bytes are in the inner stream before any newline or flush.
  EXPECT_EQ(inner.str(), "error: a");
  err << "\n\npartial";
  err.put('!');
  EXPECT_EQ(inner.str(), "error: a\n\npartial!");
  // An empty line is passed through and makes no record.
  EXPECT_EQ(tee_lines(capture), (std::vector<std::string>{"[error] elctl: error: a"}));
}

// A hub that echoes its records to stderr, with std::cerr teed into it (elctl
// with `echo_stderr = true`): each record is in the file once, not again as an
// `elctl` line, and the echo does not wait on itself.
TEST(LogTee, EchoedRecordIsNotLoggedAgain) {
  const auto dir = pychron::testing::unique_temp_path("elctl_log_tee_echo_");
  std::filesystem::create_directories(dir);
  std::string text;
  {
    const pychron::SteadyClock clock;
    pychron::config::LoggingConfig cfg;
    cfg.dir = dir;
    cfg.echo_stderr = true;
    auto hub = pychron::LogHub::create(cfg, clock);
    ASSERT_TRUE(hub);
    const pychron::ProcessLogScope scope(*hub);
    ::testing::internal::CaptureStderr();
    {
      const StreamTee tee(std::cerr, pychron::process_logger("elctl"));
      pychron::process_logger("core").warn("careful");
      std::cerr << "error: said once\n";
    }
    const std::string echoed = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(echoed.find("[warn] core: careful\n"), std::string::npos) << echoed;
    EXPECT_NE(echoed.find("[error] elctl: error: said once\n"), std::string::npos) << echoed;
    (*hub)->flush();
    std::ifstream in((*hub)->file_path());
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  std::filesystem::remove_all(dir);
  EXPECT_NE(text.find("] core: careful"), std::string::npos) << text;
  EXPECT_NE(text.find("] elctl: error: said once"), std::string::npos) << text;
  EXPECT_EQ(text.find("] elctl: ["), std::string::npos) << text;
}

}  // namespace
}  // namespace elctl::testing
