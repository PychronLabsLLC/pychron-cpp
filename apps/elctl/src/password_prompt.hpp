#pragma once

// A password asked for at a terminal, without echo: `elctl earthbank login`
// and `elctl data-source set --server` (built with or without the store).

#include <optional>
#include <string>
#include <string_view>

#include "cli.hpp"

namespace elctl {

// Whether the program's input is a terminal a password can be typed at
// without echo; and that password, read. Null if it could not be read.
bool at_terminal(Io io);
std::optional<std::string> read_password(Io io, std::string_view prompt);

}  // namespace elctl
