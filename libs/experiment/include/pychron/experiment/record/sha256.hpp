#pragma once

#include <string>
#include <string_view>

namespace pychron::experiment::record {

// Lower-case hex SHA-256 of `data`.
std::string sha256_hex(std::string_view data);

}  // namespace pychron::experiment::record
