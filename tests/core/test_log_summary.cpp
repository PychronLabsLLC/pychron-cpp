#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "pychron/core/log_summary.hpp"

using namespace pychron;

using Fields = std::vector<std::pair<std::string, std::string>>;

TEST(LogSummary, RedactsSecrets) {
  const Fields f{{"user", "jake"}, {"password", "p"}, {"api_key", "k"}};
  EXPECT_EQ(summarize(f), "user=jake, password=***, api_key=***");
}

TEST(LogSummary, RedactionIsCaseInsensitiveAndKeepsLookalikes) {
  const Fields f{{"Token", "a"}, {"auth-secret", "b"}, {"keyboard", "c"}, {"monkey", "d"}, {"KEY", "e"}};
  EXPECT_EQ(summarize(f), "Token=***, auth-secret=***, keyboard=c, monkey=d, KEY=***");
}

TEST(LogSummary, EmptyIsEmpty) { EXPECT_EQ(summarize(Fields{}), ""); }

TEST(LogSummary, CutsAt200) {
  const std::string ellipsis = "\xE2\x80\xA6";
  const Fields f{{"a", std::string(500, 'x')}};
  const std::string out = summarize(f);
  EXPECT_LE(out.size(), 200 + ellipsis.size());
  EXPECT_EQ(out.substr(out.size() - ellipsis.size()), ellipsis);
}

TEST(LogSummary, TruncateKeepsShortText) { EXPECT_EQ(truncate_for_log("abc", 5), "abc"); }

TEST(LogSummary, TruncateDoesNotSplitMultibyte) {
  // "é" is two bytes; a cut after one byte would split it.
  const std::string text = "ab\xC3\xA9\xC3\xA9";
  EXPECT_EQ(truncate_for_log(text, 3), "ab\xE2\x80\xA6");
  EXPECT_EQ(truncate_for_log(text, 4), "ab\xC3\xA9\xE2\x80\xA6");
}
