#include <gtest/gtest.h>

#include "pychron/core/calendar.hpp"

using pychron::is_calendar_date;

TEST(Calendar, AcceptsDaysOfTheCalendar) {
  for (const char* day : {"2016-02-29", "2000-02-29", "1999-12-31", "0001-01-01", "9999-12-31", "2019-03-04"})
    EXPECT_TRUE(is_calendar_date(day)) << day;
}

TEST(Calendar, RefusesDaysThatDoNotExist) {
  for (const char* day : {"2015-02-29", "1900-02-29", "2016-02-30", "2016-04-31", "2016-13-01", "2016-00-10",
                          "2016-01-00", "2016-01-32", "0000-00-00", "0000-01-01"})
    EXPECT_FALSE(is_calendar_date(day)) << day;
}

TEST(Calendar, RefusesAnythingThatIsNotYearMonthDay) {
  for (const char* text : {"", "2016-2-3", "16-02-03", "2016/02/03", "2016-02-03 ", " 2016-02-03", "2016-02-03T00:00:00",
                           "2016-02-0x", "+016-02-03", "20160203", "2016-02-03Z"})
    EXPECT_FALSE(is_calendar_date(text)) << text;
}
