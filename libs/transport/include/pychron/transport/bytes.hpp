#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pychron {

// Raw wire bytes. Transports move these; only codecs know what they mean.
using Bytes = std::vector<std::uint8_t>;

// Bytes of `s`, verbatim: to_bytes("#0010\r").
Bytes to_bytes(std::string_view s);

// Bytes as a std::string, verbatim (may contain NULs).
std::string to_string(const Bytes& b);

// Lower-case hex without separators: {0x01, 0xAB} -> "01ab".
std::string to_hex(const Bytes& b);

// Inverse of to_hex (either case). nullopt on odd length or non-hex digits.
std::optional<Bytes> from_hex(std::string_view hex);

// Printable rendering for logs/errors: ASCII kept, others as \xNN, \r \n escaped.
std::string escape(const Bytes& b);

}  // namespace pychron
