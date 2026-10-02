// run_process: stdin, captured output, exit codes, environment, a missing
// program and the timeout. The commands go through the platform shell.

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/core/process.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

ProcessSpec shell(const std::string& command) {
  ProcessSpec s;
#ifdef _WIN32
  s.argv = {"cmd", "/d", "/s", "/c", command};
#else
  s.argv = {"sh", "-c", command};
#endif
  return s;
}

}  // namespace

TEST(Process, FeedsStdinAndCapturesOutputAndTheExitCode) {
#ifdef _WIN32
  auto spec = shell("sort & echo oops 1>&2 & exit 3");
#else
  auto spec = shell("cat; echo oops >&2; exit 3");
#endif
  spec.input = "hello\n";
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->exit_code, 3);
  EXPECT_NE(r->output.find("hello"), std::string::npos) << r->output;
  EXPECT_NE(r->output.find("oops"), std::string::npos) << r->output;
}

TEST(Process, ArgumentsArePassedAsTheyAre) {
  ProcessSpec spec;
#ifdef _WIN32
  spec.argv = {"cmd", "/d", "/c", "echo", "a b"};
#else
  spec.argv = {"printf", "%s|%s", "a b", "c\"d"};
#endif
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->exit_code, 0);
#ifdef _WIN32
  EXPECT_NE(r->output.find("a b"), std::string::npos) << r->output;
#else
  EXPECT_EQ(r->output, "a b|c\"d");
#endif
}

TEST(Process, AddsEnvironmentVariables) {
#ifdef _WIN32
  auto spec = shell("echo %PYCHRON_PROCESS_TEST%");
#else
  auto spec = shell("printf %s \"$PYCHRON_PROCESS_TEST\"");
#endif
  spec.env = {{"PYCHRON_PROCESS_TEST", "42"}};
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_NE(r->output.find("42"), std::string::npos) << r->output;
}

TEST(Process, AMissingProgramIsAnIoError) {
  ProcessSpec spec;
  spec.argv = {"pychron-no-such-program-xyz"};
  auto r = run_process(spec);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("pychron-no-such-program-xyz"), std::string::npos) << r.error().what;
  EXPECT_FALSE(run_process(ProcessSpec{}));
}

TEST(Process, AProgramThatRunsTooLongIsKilled) {
#ifdef _WIN32
  auto spec = shell("ping -n 30 127.0.0.1 > nul");
#else
  auto spec = shell("sleep 30; true");  // a grandchild holding the output pipe
#endif
  spec.timeout = 300ms;
  const auto t0 = std::chrono::steady_clock::now();
  auto r = run_process(spec);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 10s);
}
