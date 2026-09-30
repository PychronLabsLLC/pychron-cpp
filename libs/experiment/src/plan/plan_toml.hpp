#pragma once

// Internal helpers shared by the plan sources: problem collection and
// dotted/indexed path navigation over a toml::table.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/types.hpp"
#include "pychron/experiment/plan/plan.hpp"

namespace pychron::experiment::plan::detail {

class Problems {
 public:
  explicit Problems(std::string_view source) : source_(source) {}
  void add(std::string_view where, std::string_view msg) {
    if (!text_.empty()) text_ += '\n';
    text_ += source_;
    text_ += ": ";
    if (!where.empty()) {
      text_ += where;
      text_ += ": ";
    }
    text_ += msg;
  }
  bool any() const { return !text_.empty(); }
  Unexpected<Error> error() const { return fail(ErrorKind::Config, text_); }

 private:
  std::string source_;
  std::string text_;
};

// "main.hops[0].counts" -> {main}, {hops, 0}, {counts}.
struct PathSegment {
  std::string key;
  std::optional<std::size_t> index;
};

std::optional<std::vector<PathSegment>> parse_path(std::string_view path);

// The node at `path`, or null. Missing keys, out-of-range indices and
// indexing a non-array all yield null; `why` then says which.
toml::node* find_node(toml::table& root, const std::vector<PathSegment>& path, std::string* why = nullptr);
const toml::node* find_node(const toml::table& root, std::string_view path);

// True when `path` names a node, or a missing key directly under an existing table.
bool path_addressable(const toml::table& root, std::string_view path);

// parameters.expose entries: "path" or { path, label }; paths must be addressable.
std::vector<ExposeEntry> read_expose(const toml::table& root, Problems& problems);

// "a.b[2]" -> "a.b[2].key" / "a.b[2][3]".
std::string join(std::string_view base, std::string_view key);
std::string join(std::string_view base, std::size_t index);

bool is_alias(const toml::node& n);

}  // namespace pychron::experiment::plan::detail
