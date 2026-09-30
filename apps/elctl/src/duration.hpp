#pragma once

#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"

namespace elctl {

// "250ms", "3s", "1.5s", "2m", "1h"; a bare number is seconds. Config error
// for anything else, including negative values.
pychron::Result<pychron::Duration> parse_duration(std::string_view text);

}  // namespace elctl
