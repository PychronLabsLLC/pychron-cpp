#pragma once

#include <string_view>

namespace pychron {

// True when `text` is "YYYY-MM-DD" and names a day of the (proleptic
// Gregorian) calendar, year 0001 or later. Nothing else is accepted: no
// surrounding space, no time, no other separators.
bool is_calendar_date(std::string_view text);

}  // namespace pychron
