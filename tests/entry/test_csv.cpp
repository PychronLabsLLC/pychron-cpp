#include <gtest/gtest.h>

#include "pychron/entry/csv.hpp"

using namespace pychron::entry;

TEST(Csv, QuotesDelimitersAndNewlines) {
  auto t = read_csv("a,b,c\n\"x, y\",\"say \"\"hi\"\"\",\"two\nlines\"\nplain,,end\n");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->header, (std::vector<std::string>{"a", "b", "c"}));
  ASSERT_EQ(t->rows.size(), 2u);
  EXPECT_EQ(t->rows[0], (std::vector<std::string>{"x, y", "say \"hi\"", "two\nlines"}));
  EXPECT_EQ(t->rows[1], (std::vector<std::string>{"plain", "", "end"}));
  EXPECT_EQ(t->lines, (std::vector<int>{2, 4}));
}

TEST(Csv, BomCrlfAndBlankLines) {
  auto t = read_csv("\xEF\xBB\xBFsample;lat\r\nA;1.5\r\n\r\nB;2\r\n\r\n");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->delimiter, ';');
  EXPECT_EQ(t->header, (std::vector<std::string>{"sample", "lat"}));
  ASSERT_EQ(t->rows.size(), 2u);
  EXPECT_EQ(t->rows[1][0], "B");
  EXPECT_EQ(t->lines[1], 4);
}

TEST(Csv, Sniffing) {
  EXPECT_EQ(sniff_delimiter("a\tb\tc\n1,2\n"), '\t');
  EXPECT_EQ(sniff_delimiter("a|b\n"), '|');
  EXPECT_EQ(sniff_delimiter("\"a,b\";c;d\n"), ';');
  EXPECT_EQ(sniff_delimiter("a,b;c\n"), ',');  // a tie goes to ','
  EXPECT_EQ(sniff_delimiter("single\n"), ',');
}

TEST(Csv, RaggedRowsAreReported) {
  auto t = read_csv("a,b,c\n1,2\n1,2,3,4\n1,2,3\n");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->ragged, (std::vector<int>{2, 3}));
  EXPECT_EQ(t->rows[0].size(), 3u);  // padded
}

TEST(Csv, Errors) {
  EXPECT_FALSE(read_csv(""));
  EXPECT_FALSE(read_csv("a,b\n\"open,1\n"));
}

TEST(Csv, WriteReadsBack) {
  const std::vector<std::string> header{"a", "b"};
  const std::vector<std::vector<std::string>> rows{{"x,y", "q\"q"}, {"line\nbreak", ""}};
  auto t = read_csv(write_csv(header, rows));
  ASSERT_TRUE(t);
  EXPECT_EQ(t->header, header);
  EXPECT_EQ(t->rows, rows);
}
