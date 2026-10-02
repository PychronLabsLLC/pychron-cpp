#pragma once

// Options (design section 6): a schema describes one kind of options as data;
// Options is a value tree read through the schema; PresetStore keeps named
// presets as versioned TOML files.
//
// Schemas are static and must outlive every Options that refers to them
// (Options holds a shared_ptr to its schema).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::processing {

// monostate = unset (legal only for optional fields).
using OptionValue = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>>;

enum class FieldType {
  Bool,
  Int,
  Double,
  String,
  Enum,        // one of choices
  Color,       // "#rrggbb" or "#rrggbbaa"
  Quantity,    // a quantity expression (quantity.hpp)
  StringList,  // array of strings
};

std::string_view to_string(FieldType type) noexcept;

struct FieldSpec {
  std::string key;      // dotted: "x.time_format" is [x] time_format in TOML
  std::string label;
  std::string section;  // dock tab
  FieldType type = FieldType::String;
  OptionValue default_value;
  bool optional = false;  // unset is legal; the UI shows a check box
  double min = -1e300, max = 1e300, step = 0;
  std::vector<std::string> choices;  // Enum
  std::string help;
  std::string enabled_when;  // "<key> == <value>" or "<key> != <value>" or "<key>" (a bool), for the UI
};

class Options;
class Schema;
using SchemaPtr = std::shared_ptr<const Schema>;

struct ListSpec {
  std::string key, label, section;
  SchemaPtr row;  // fields of one row
  std::size_t min_rows = 0, max_rows = 64;
  std::vector<std::string> default_rows_toml;  // each row as TOML key = value lines
};

// version n -> n + 1, applied to the raw values before validation.
using Migration = std::function<void(Options&)>;

class Schema {
 public:
  std::string kind;  // "figure.time_series"
  std::string title;
  int version = 1;
  std::vector<FieldSpec> fields;
  std::vector<ListSpec> lists;
  std::vector<Migration> migrations;  // migrations[i]: version i + 1 -> i + 2
  // Compiled-in presets: name -> TOML text (without schema/version/name).
  std::vector<std::pair<std::string, std::string>> factory_presets;

  const FieldSpec* field(std::string_view key) const;
  const ListSpec* list(std::string_view key) const;
};

class Options {
 public:
  Options() = default;
  explicit Options(SchemaPtr schema) : schema_(std::move(schema)) {}

  const SchemaPtr& schema() const noexcept { return schema_; }

  // The stored value, or the schema default. Keys must be in the schema
  // (debug assert; monostate in release).
  OptionValue get(std::string_view key) const;
  bool is_set(std::string_view key) const;
  bool get_bool(std::string_view key) const;
  std::int64_t get_int(std::string_view key) const;
  double get_double(std::string_view key) const;  // Int values convert
  std::optional<double> get_optional_double(std::string_view key) const;
  std::string get_string(std::string_view key) const;  // String, Enum, Color, Quantity
  std::vector<std::string> get_strings(std::string_view key) const;

  // Validated against the schema: type, choices, bounds, colour syntax,
  // quantity grammar. monostate unsets (optional fields only).
  Result<void> set(std::string_view key, OptionValue value);
  void unset(std::string_view key);

  // Rows of a list; the schema's default rows when never set.
  std::vector<Options> rows(std::string_view key) const;
  Result<void> set_rows(std::string_view key, std::vector<Options> rows);
  Options new_row(std::string_view list_key) const;  // empty row with the row schema

  // Unknown keys read from a file, written back unchanged.
  std::map<std::string, OptionValue> extra;
  std::map<std::string, std::vector<Options>> extra_lists;

  // Every explicitly set value, sorted, one per line; rows recurse. Two
  // Options with the same values have the same canonical text.
  std::string canonical() const;

  // Raw access for migrations and I/O (no validation).
  std::map<std::string, OptionValue>& raw_values() noexcept { return values_; }
  const std::map<std::string, OptionValue>& raw_values() const noexcept { return values_; }
  std::map<std::string, std::vector<Options>>& raw_lists() noexcept { return lists_; }
  const std::map<std::string, std::vector<Options>>& raw_lists() const noexcept { return lists_; }

  friend bool operator==(const Options& a, const Options& b) { return a.canonical() == b.canonical(); }

 private:
  SchemaPtr schema_;
  std::map<std::string, OptionValue> values_;
  std::map<std::string, std::vector<Options>> lists_;
};

// Validates a value for a field (also used by the UI before set()).
Result<OptionValue> validate(const FieldSpec& field, const OptionValue& value);
// "#rrggbb" / "#rrggbbaa" (case-insensitive) or a palette name.
bool is_color(std::string_view text);

// ---- TOML -------------------------------------------------------------------

struct LoadedOptions {
  Options options;
  std::string name;             // the file's name key, if any
  int file_version = 0;
  std::vector<std::string> warnings;  // dropped bad values, newer version, ...
};

// Parses a preset/options document: `schema`, `version`, `name` header keys,
// then the values. A wrong `schema` key is an error; missing header keys are
// allowed (factory texts). Runs migrations, validates every value (a bad one
// is dropped with a warning), keeps unknown keys in extra.
Result<LoadedOptions> options_from_toml(const SchemaPtr& schema, std::string_view text);
// Writes the header and every set value; dotted keys become tables.
std::string options_to_toml(const Options& options, std::string_view name = {});

// ---- Presets ----------------------------------------------------------------

enum class PresetOrigin { Factory, Lab, User };

struct PresetInfo {
  std::string name;
  PresetOrigin origin = PresetOrigin::User;
  std::filesystem::path path;  // empty for factory presets
  bool shadows = false;        // a user/lab preset with a factory or lab name
};

// Presets under <root>/<kind>/<file>.toml. Layering: factory < lab < user;
// the highest layer with a name wins. Names keep their case; the file name is
// a sanitized form and the real name is stored in the file.
class PresetStore {
 public:
  explicit PresetStore(std::filesystem::path user_root, std::optional<std::filesystem::path> lab_root = {});

  std::vector<PresetInfo> list(const SchemaPtr& schema) const;  // sorted by name, highest layer only
  Result<LoadedOptions> load(const SchemaPtr& schema, std::string_view name) const;
  Result<LoadedOptions> factory(const SchemaPtr& schema, std::string_view name) const;
  // The first factory preset, or the schema defaults when there is none.
  Options defaults(const SchemaPtr& schema) const;

  Result<void> save(std::string_view name, const Options& options) const;  // user layer
  Result<void> remove(const SchemaPtr& schema, std::string_view name) const;  // user layer only
  Result<void> rename(const SchemaPtr& schema, std::string_view from, std::string_view to) const;

  static std::string file_stem(std::string_view name);  // sanitized, lower-case
  static Result<void> check_name(std::string_view name);

 private:
  std::filesystem::path dir(const std::filesystem::path& root, const Schema& schema) const;

  std::filesystem::path user_root_;
  std::optional<std::filesystem::path> lab_root_;
};

}  // namespace pychron::processing
