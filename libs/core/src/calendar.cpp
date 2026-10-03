#include "pychron/core/calendar.hpp"

#include <chrono>
#include <cstddef>

namespace pychron {

bool is_calendar_date(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
  int parts[3] = {0, 0, 0};
  const std::size_t starts[3] = {0, 5, 8}, lengths[3] = {4, 2, 2};
  for (std::size_t p = 0; p < 3; ++p)
    for (std::size_t i = 0; i < lengths[p]; ++i) {
      const char c = text[starts[p] + i];
      if (c < '0' || c > '9') return false;
      parts[p] = parts[p] * 10 + (c - '0');
    }
  const std::chrono::year_month_day day{std::chrono::year{parts[0]}, std::chrono::month{static_cast<unsigned>(parts[1])},
                                        std::chrono::day{static_cast<unsigned>(parts[2])}};
  return parts[0] >= 1 && day.ok();
}

}  // namespace pychron
