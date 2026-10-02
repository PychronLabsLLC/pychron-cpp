#include "pychron/experiment/record/sha256.hpp"

#include "pychron/core/sha256.hpp"

namespace pychron::experiment::record {

std::string sha256_hex(std::string_view data) { return to_hex(sha256(data)); }

}  // namespace pychron::experiment::record
