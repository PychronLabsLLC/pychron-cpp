#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "pychron/core/log_context.hpp"

using namespace pychron;

TEST(LogContext, ThreadTagScoped) {
  // A fresh thread, so a tag left by another test cannot leak in.
  std::thread worker([] {
    EXPECT_EQ(LogContext::thread_tag(), "");
    {
      const LogContext::Thread outer{"executor"};
      EXPECT_EQ(LogContext::thread_tag(), "executor");
      {
        const LogContext::Thread inner{"script"};
        EXPECT_EQ(LogContext::thread_tag(), "script");
      }
      EXPECT_EQ(LogContext::thread_tag(), "executor");
    }
    EXPECT_EQ(LogContext::thread_tag(), "");
  });
  worker.join();
}

TEST(LogContext, RunNestsAndRestores) {
  std::thread worker([] {
    EXPECT_EQ(LogContext::run_id(), "");
    {
      const LogContext::Run outer{"12345-01A"};
      EXPECT_EQ(LogContext::run_id(), "12345-01A");
      {
        const LogContext::Run inner{"12345-01B"};
        EXPECT_EQ(LogContext::run_id(), "12345-01B");
      }
      EXPECT_EQ(LogContext::run_id(), "12345-01A");
    }
    EXPECT_EQ(LogContext::run_id(), "");
  });
  worker.join();
}

TEST(LogContext, ThreadNumbersDiffer) {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::thread ta([&a] { a = LogContext::thread_number(); });
  std::thread tb([&b] { b = LogContext::thread_number(); });
  ta.join();
  tb.join();
  EXPECT_GE(a, 1u);
  EXPECT_GE(b, 1u);
  EXPECT_NE(a, b);
  // Stable for the life of a thread.
  const std::uint32_t mine = LogContext::thread_number();
  EXPECT_EQ(LogContext::thread_number(), mine);
}

TEST(LogContext, PollingNests) {
  std::thread worker([] {
    EXPECT_FALSE(LogContext::polling());
    {
      const LogContext::Polling outer;
      EXPECT_TRUE(LogContext::polling());
      {
        const LogContext::Polling inner;
        EXPECT_TRUE(LogContext::polling());
      }
      EXPECT_TRUE(LogContext::polling());
    }
    EXPECT_FALSE(LogContext::polling());
  });
  worker.join();
}
