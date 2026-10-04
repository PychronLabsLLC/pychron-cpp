#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace pychron::experiment::detail {

// The shortest of %.15g / %.16g / %.17g that reads back as the same double.
inline std::string shortest(double v) {
  char b[40];
  for (int digits = 15; digits <= 17; ++digits) {
    std::snprintf(b, sizeof b, "%.*g", digits, v);
    if (std::strtod(b, nullptr) == v) break;
  }
  return b;
}

}  // namespace pychron::experiment::detail
