// Every script and host call in the log (global logging, phase 2): the lines a
// script run leaves on `script.<name>`.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "host_call_trace.hpp"
#include "log_capture.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/scripting/script_host.hpp"

using namespace pychron;
using namespace pychron::scripting;
using namespace pychron::scripting::testing;

namespace {

// The messages of the `script.inline.py` records, in order.
std::vector<std::string> script_records(const pychron::testing::LogCapture& capture) {
  std::vector<std::string> out;
  for (const auto& line : capture.lines()) {
    const std::string marker = "script.inline.py: ";
    if (auto at = line.find(marker); at != std::string::npos)
      out.push_back(line.substr(0, line.find(' ')) + " " + line.substr(at + marker.size()));
  }
  return out;
}

bool starts_with_at(const std::vector<std::string>& records, std::size_t i, const std::string& prefix) {
  return i < records.size() && records[i].find(prefix) != std::string::npos &&
         records[i].find(prefix) <= 8;
}

class ScriptLog : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
    ASSERT_TRUE(capture.ok());
    host = make_script_host();
  }
  pychron::testing::LogCapture capture;
  Rig rig;
  CancelToken token;
  std::unique_ptr<IScriptHost> host;
};

}  // namespace

#if PYCHRON_SCRIPTING_ENABLED

TEST_F(ScriptLog, StartCallsEnd) {
  Script script{"inline.py", "def main():\n    open('A')\n    sleep(0.01)\n    close('A')\n",
                ScriptKind::Extraction};
  auto r = host->run(script, rig.env, token);
  ASSERT_TRUE(r.has_value());
  auto records = script_records(capture);
  ASSERT_EQ(records.size(), 5u) << ::testing::PrintToString(records);
  EXPECT_TRUE(starts_with_at(records, 0, "[debug] start inline.py path=inline.py sha=" + r->sha))
      << records[0];
  EXPECT_NE(records[0].find(" kind=extraction"), std::string::npos) << records[0];
  EXPECT_TRUE(starts_with_at(records, 1, "[debug] open(name=A) -> ok in ")) << records[1];
  EXPECT_TRUE(starts_with_at(records, 2, "[debug] sleep(seconds=0.01) -> ok in ")) << records[2];
  EXPECT_TRUE(starts_with_at(records, 3, "[debug] close(name=A) -> ok in ")) << records[3];
  EXPECT_TRUE(starts_with_at(records, 4, "[debug] end inline.py completed in ")) << records[4];
}

TEST_F(ScriptLog, FailedCallIsWarn) {
  rig.valves.interlocked.insert("A");
  Script script{"inline.py", "def main():\n    open('A')\n", ScriptKind::Extraction};
  auto r = host->run(script, rig.env, token);
  ASSERT_FALSE(r.has_value());
  auto records = script_records(capture);
  ASSERT_EQ(records.size(), 3u) << ::testing::PrintToString(records);
  EXPECT_TRUE(starts_with_at(records, 1, "[warn] open(name=A) -> error: interlock: interlocked (A) in "))
      << records[1];
  EXPECT_TRUE(starts_with_at(records, 2, "[warn] end inline.py error: interlock: interlocked (A) in "))
      << records[2];
}

TEST_F(ScriptLog, ReadsLogTheirAnswer) {
  Script script{"inline.py", "def main():\n    get_pressure('c', 'g')\n    is_open('A')\n",
                ScriptKind::Extraction};
  ASSERT_TRUE(host->run(script, rig.env, token).has_value());
  auto records = script_records(capture);
  ASSERT_EQ(records.size(), 4u) << ::testing::PrintToString(records);
  EXPECT_TRUE(starts_with_at(records, 1, "[debug] get_pressure(controller=c, gauge=g) -> 1e-08 in "))
      << records[1];
  EXPECT_TRUE(starts_with_at(records, 2, "[debug] is_open(name=A) -> false in ")) << records[2];
}

// No host call takes a password: the redaction is the summary's, reached through
// the trace with a call that would.
TEST(ScriptLogTrace, SecretArgumentRedacted) {
  pychron::testing::LogCapture capture;
  ASSERT_TRUE(capture.ok());
  SteadyClock clock;
  auto logger = process_logger("script.secret.py");
  {
    python::HostCallTrace trace(logger, clock, "login",
                                [] { return python::HostCallTrace::Args{{"user", "ann"}, {"password", "hunter2"}}; });
  }
  bool seen = false;
  for (const auto& line : capture.lines()) {
    EXPECT_EQ(line.find("hunter2"), std::string::npos) << line;
    if (line.find("login(user=ann, password=***) -> ok in ") != std::string::npos) seen = true;
  }
  EXPECT_TRUE(seen);
}

TEST(ScriptLogTrace, SummaryNotBuiltWhenDisabled) {
  SteadyClock clock;  // no process hub: the logger is disabled
  auto logger = process_logger("script.none.py");
  bool built = false;
  {
    python::HostCallTrace trace(logger, clock, "open", [&] {
      built = true;
      return python::HostCallTrace::Args{{"name", "A"}};
    });
  }
  EXPECT_FALSE(built);
}

#endif  // PYCHRON_SCRIPTING_ENABLED

TEST(ScriptLogNoHub, NoHubNoCost) {
  if (!scripting_enabled()) GTEST_SKIP() << "built without PYCHRON_SCRIPTING";
  ASSERT_EQ(process_log_hub(), nullptr);
  Rig rig;
  CancelToken token;
  auto host = make_script_host();
  Script script{"inline.py", "def main():\n    open('A')\n    close('A')\n", ScriptKind::Extraction};
  EXPECT_TRUE(host->run(script, rig.env, token).has_value());
  EXPECT_TRUE(rig.log.contains("open A"));
}
