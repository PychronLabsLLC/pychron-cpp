#pragma once

// The tolerant JSON reader behind the legacy parsers. Private to pychron_dvc:
// nlohmann_json must not reach a public header.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"

namespace pychron::dvc {

using Json = nlohmann::json;  // objects keep their keys sorted, so dump() is deterministic

// A bare NaN, Infinity or -Infinity that was in the text.
struct NonFinite {
  std::string pointer;  // JSON pointer of the value, e.g. "/Ar40/value"
  std::string token;    // "NaN", "Infinity" or "-Infinity"
};

// Parses what Python's json module writes: standard JSON plus the bare tokens
// NaN, Infinity and -Infinity. Outside strings each token becomes null and,
// when `nonfinite` is given, is reported there; the same letters inside a
// string are untouched. A UTF-8 BOM is skipped and CRLF is whitespace.
// Anything else that is not JSON (including an empty or truncated text) is an
// error.
Result<Json> parse_legacy(std::string_view text, std::vector<NonFinite>* nonfinite = nullptr);

// Compact text of `j`.
std::string dump(const Json& j);

// Conversions that accept what real files contain. nullopt: not convertible.
std::optional<double> as_double(const Json& j);    // a number, or a string that is one ("3.0")
std::optional<int> as_int(const Json& j);          // an integer, an integral float, or a string of one ("25")
std::optional<bool> as_bool(const Json& j);        // a boolean, 0 or 1, "true" or "false"
std::optional<std::string> as_text(const Json& j);  // a string; a number or boolean as its JSON text

// take_*: move `key` out of the object into a typed field. The key is erased
// when its value is represented by the result: converted, or null or "" (both
// mean "not set" and give nullopt). A value that cannot be converted stays in
// the object, so that it ends up in the caller's extra.
std::optional<double> take_double(Json& object, std::string_view key);
std::optional<int> take_int(Json& object, std::string_view key);
std::optional<bool> take_bool(Json& object, std::string_view key);
std::optional<std::string> take_text(Json& object, std::string_view key);
// The value as JSON text; null gives nullopt. Always erased.
std::optional<std::string> take_json(Json& object, std::string_view key);

// The entries of `nonfinite` under `prefix` (a JSON pointer; "" for all), as
// an object {"<pointer relative to prefix>": "<token>"}; null when there are
// none.
Json nonfinite_under(const std::vector<NonFinite>& nonfinite, std::string_view prefix);

// JSON pointer of a top-level key ("/L2(CDD)"; '~' and '/' escaped).
std::string pointer_of(std::string_view key);

// `object` as text for an extra column: nullopt when it is null or an empty
// object.
std::optional<std::string> extra_text(const Json& object);

}  // namespace pychron::dvc
