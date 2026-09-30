#pragma once

// Private schema-checking helpers shared by the spectrometer config, table
// and molecular-weight loaders. Every reader reports into one diagnostics
// list and keeps going, so a file's problems are collected in one pass.

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/config/system_config.hpp"

namespace pychron::spectrometer::cfg::detail {

using config::Diagnostic;
using config::Located;
using config::SourceLoc;

std::string_view type_name(toml::node_type t);

// One table being read: its dotted path ("" at file root), its location and,
// optionally, the entity recording per-key locations.
struct Obj {
  const toml::table& t;
  std::string path;
  SourceLoc loc;
  Located* rec = nullptr;
};

template <class E>
using Choices = std::span<const std::pair<std::string_view, E>>;

class Reader {
 public:
  Reader(std::string file, std::vector<Diagnostic>& out) : file_(std::move(file)), out_(out) {}

  const std::string& file() const { return file_; }
  SourceLoc loc(const toml::node& n) const;
  void error(SourceLoc where, std::string field, std::string message);
  static std::string join(const std::string& path, std::string_view key);

  // Table at `n`, or an "expected table" error and nullptr.
  const toml::table* table(const toml::node& n, const std::string& path);
  Obj obj(const toml::table& t, std::string path, Located* rec = nullptr);

  // Reports every key of `o` not in `keys`.
  void only(const Obj& o, std::initializer_list<std::string_view> keys);

  // Node for `key`; reports a missing required key.
  const toml::node* get(const Obj& o, std::string_view key, bool required);

  bool str(const Obj& o, std::string_view key, std::string& out, bool required);
  bool integer(const Obj& o, std::string_view key, std::int64_t& out, bool required, std::int64_t min);
  bool number(const Obj& o, std::string_view key, double& out, bool required);
  bool number(const Obj& o, std::string_view key, std::optional<double>& out);
  bool boolean(const Obj& o, std::string_view key, bool& out);
  bool strings(const Obj& o, std::string_view key, std::vector<std::string>& out, bool required);
  bool numbers(const Obj& o, std::string_view key, std::vector<double>& out);

  // Nested inline/standard table under `key`.
  std::optional<Obj> sub(const Obj& o, std::string_view key, bool required, Located* rec = nullptr);

  template <class E>
  bool choice(const Obj& o, std::string_view key, E& out, Choices<E> choices, bool required) {
    std::string s;
    if (!str(o, key, s, required)) return false;
    if (o.t.get(key) == nullptr) return true;
    for (const auto& [name, value] : choices) {
      if (name == s) {
        out = value;
        return true;
      }
    }
    std::string names;
    for (const auto& c : choices) names += (names.empty() ? "" : ", ") + std::string(c.first);
    error(loc(*o.t.get(key)), join(o.path, key), "'" + s + "' must be one of: " + names);
    return false;
  }

  // Value-level checks that report against `key`'s location.
  void require(bool ok, const Obj& o, std::string_view key, std::string message);

  static std::optional<double> as_number(const toml::node& n);

 private:
  void type_error(const Obj& o, std::string_view key, const toml::node& n, std::string_view expected);

  std::string file_;
  std::vector<Diagnostic>& out_;
};

std::optional<toml::table> parse_toml(std::string_view text, std::string_view name, std::vector<Diagnostic>& out);
std::optional<std::string> read_file(const std::filesystem::path& path);

}  // namespace pychron::spectrometer::cfg::detail
