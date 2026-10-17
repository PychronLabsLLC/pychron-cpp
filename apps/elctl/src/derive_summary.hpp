#pragma once

// The counts of a pass that fills the cache of ages (age cache spec 4.5), as
// `elctl db derive` and `elctl import run` print them.

#include <string>

#include "pychron/processing/derive.hpp"

namespace elctl {

inline std::string derive_counts(const pychron::processing::DeriveStats& s) {
  return std::to_string(s.seen) + " seen, " + std::to_string(s.written) + " written (" + std::to_string(s.no_age) +
         " without an age), " + std::to_string(s.current) + " current, " + std::to_string(s.failed) + " failed";
}

}  // namespace elctl
