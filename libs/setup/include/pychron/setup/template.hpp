#pragma once

// The small template language setup profiles are written in (installation
// wizard spec section 3.1). Deliberately tiny: substitution, one filter,
// conditions and loops, nothing else.
//
//   {{ name }}            the value as text (lists comma-separated)
//   {{ name | toml }}     the value as a TOML value: "quoted string", 42,
//                         1.5, true, ["a", "b"]
//   {{ row.field }}       a field of a table row inside a loop
//   {% if cond %} ... {% else %} ... {% endif %}
//   {% for x in name %} ... {% endfor %}   over a list or a table's rows
//
// Conditions: `name` (truthy: true, non-empty, non-zero), `not name`,
// `name == "text"`, `name != "text"`, and the same with row fields. A line
// holding only a {% %} tag (and whitespace) is removed with its newline, so
// block tags leave no blank lines in the output. An unknown name, an unclosed
// block or a malformed tag is an error naming the template and the line.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::setup {

// One row of a table answer (e.g. a detector: name, kind, isotope).
using Row = std::map<std::string, std::string>;

using Value = std::variant<bool, std::int64_t, double, std::string, std::vector<std::string>, std::vector<Row>>;
using Answers = std::map<std::string, Value>;

// "true", "42", "1.5", "text", "a, b"; table answers as their row count.
std::string to_text(const Value& v);
// A TOML literal for `v`; a table answer is an array of inline tables.
std::string to_toml(const Value& v);
bool truthy(const Value& v);

Result<std::string> render(std::string_view text, const Answers& answers, std::string_view name = "template");

// A condition as used in {% if %} and in profile `when` keys.
Result<bool> evaluate(std::string_view condition, const Answers& answers);

}  // namespace pychron::setup
