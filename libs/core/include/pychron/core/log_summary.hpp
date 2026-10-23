#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pychron {

// `key=value` pairs joined by ", ". The value of a key named password, token,
// secret or key is replaced by ***. A key matches when, compared without
// regard to case, it equals one of those names or ends with "_<name>" or
// "-<name>" (api_key, auth-token); keyboard and monkey do not match. The
// result is cut as truncate_for_log does.
[[nodiscard]] std::string summarize(std::span<const std::pair<std::string, std::string>> fields);

// `text` cut to at most `max` bytes, with "…" (UTF-8) appended when anything
// was cut. The cut never splits a multibyte sequence, so the result is at most
// `max` plus three bytes.
[[nodiscard]] std::string truncate_for_log(std::string_view text, std::size_t max = 200);

}  // namespace pychron
