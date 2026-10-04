#pragma once

// Numbers in conditionals text, independent of the C locale (Qt sets it from
// the environment, and a comma decimal point would corrupt a written file).

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <string>

namespace pychron::experiment::detail {

// The shortest of %.15g / %.16g / %.17g that reads back as the same double,
// with '.' as the decimal point.
inline std::string shortest(double v) {
  char b[40];
  for (int digits = 15; digits <= 17; ++digits) {
    std::snprintf(b, sizeof b, "%.*g", digits, v);
    if (std::strtod(b, nullptr) == v) break;
  }
  std::string s = b;
  const std::string point = std::localeconv()->decimal_point;
  if (point != ".")
    if (const auto at = s.find(point); at != std::string::npos) s.replace(at, point.size(), ".");
  return s;
}

// A finite number written with '.', and nothing else.
inline bool parse_number(const std::string& s, double& out) {
  std::istringstream in(s);
  in.imbue(std::locale::classic());
  double v = 0;
  if (!(in >> v) || !std::isfinite(v)) return false;
  in >> std::ws;
  if (!in.eof()) return false;
  out = v;
  return true;
}

}  // namespace pychron::experiment::detail
