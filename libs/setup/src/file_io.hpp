#pragma once

// File writing shared inside libs/setup (not public API): the installer and
// a data source change write a profile's files the same way.

#include <filesystem>
#include <string>

#include "pychron/core/error.hpp"

namespace pychron::setup::detail {

// Writes `content` to `p`, making its folder. A secret file is made empty and
// owner-only (read and write) before the content goes in.
Result<void> write_file(const std::filesystem::path& p, const std::string& content, bool secret);

}  // namespace pychron::setup::detail
