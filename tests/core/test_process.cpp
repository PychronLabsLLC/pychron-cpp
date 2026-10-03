// run_process: stdin, captured output, exit codes, environment, a missing
// program and the timeout. The commands go through the platform shell.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

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

namespace {

std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::filesystem::path scratch(const char* name) {
  return std::filesystem::temp_directory_path() / (std::string("pychron-process-test-") + name);
}

}  // namespace

TEST(Process, StdoutFileIsUncapped) {
  const auto out = scratch("uncapped.out");
  ProcessSpec spec;
#ifdef _WIN32
  // `type` of a 1 MiB file: cmd is the only program a runner is sure to have.
  const auto src = scratch("uncapped.src");
  {
    std::ofstream f(src, std::ios::binary);
    f << std::string(1048576, 'x');
  }
  spec.argv = {"cmd", "/d", "/c", "type", src.string()};  // no shell string, so no hand-written quotes
#else
  spec = shell("head -c 1048576 /dev/zero");
#endif
  spec.stdout_file = out;
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_TRUE(r->output.empty()) << r->output;
  EXPECT_EQ(std::filesystem::file_size(out), 1048576u);
  std::filesystem::remove(out);
#ifdef _WIN32
  std::filesystem::remove(src);
#endif
}

TEST(Process, StdoutFileKeepsStderrSeparate) {
  const auto out = scratch("separate.out");
#ifdef _WIN32
  auto spec = shell("echo to-stdout & echo to-stderr 1>&2");
#else
  auto spec = shell("echo to-stdout; echo to-stderr >&2");
#endif
  spec.stdout_file = out;
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_NE(r->output.find("to-stderr"), std::string::npos) << r->output;
  EXPECT_EQ(r->output.find("to-stdout"), std::string::npos) << r->output;
  const auto content = slurp(out);
  EXPECT_NE(content.find("to-stdout"), std::string::npos) << content;
  EXPECT_EQ(content.find("to-stderr"), std::string::npos) << content;
  std::filesystem::remove(out);
}

TEST(Process, StdinStillFed) {
  const auto out = scratch("stdin.out");
  ProcessSpec spec;
#ifdef _WIN32
  spec.argv = {"findstr", "^"};
#else
  spec.argv = {"cat"};
#endif
  spec.input = "abc\n";
  spec.stdout_file = out;
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_TRUE(r->output.empty()) << r->output;
  EXPECT_EQ(slurp(out).substr(0, 3), "abc");
  std::filesystem::remove(out);
}

TEST(Process, StdoutFileIsTruncatedAndAnUnopenableOneIsAnIoError) {
  const auto out = scratch("truncated.out");
  {
    std::ofstream f(out, std::ios::binary);
    f << std::string(100, 'z');
  }
  auto spec = shell("echo hi");
  spec.stdout_file = out;
  auto r = run_process(spec);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_LT(std::filesystem::file_size(out), 10u);
  std::filesystem::remove(out);

  spec.stdout_file = scratch("no-such-dir") / "x" / "out";
  auto bad = run_process(spec);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, ErrorKind::Io);
}

TEST(Process, StdoutFileKeepsWhatWasWrittenOnTimeout) {
  const auto out = scratch("timeout.out");
#ifdef _WIN32
  auto spec = shell("echo partial & ping -n 30 127.0.0.1 > nul");
#else
  auto spec = shell("echo partial; sleep 30; true");
#endif
  spec.stdout_file = out;
  spec.timeout = 500ms;
  auto r = run_process(spec);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_NE(slurp(out).find("partial"), std::string::npos);
  std::filesystem::remove(out);
}
