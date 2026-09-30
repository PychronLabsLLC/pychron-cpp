#include "pychron/devices/driver_registry.hpp"

#include <algorithm>
#include <sstream>

namespace pychron {

namespace {

bool is_common_key(std::string_view key) { return key == "kind" || key == "transport"; }

bool all_elements(const toml::array& array, bool (toml::node::*is)() const noexcept) {
  return std::all_of(array.begin(), array.end(), [is](const toml::node& n) { return (n.*is)(); });
}

bool matches(const toml::node& node, KeyType type) {
  switch (type) {
    case KeyType::String: return node.is_string();
    case KeyType::Integer: return node.is_integer();
    case KeyType::Float: return node.is_floating_point() || node.is_integer();
    case KeyType::Boolean: return node.is_boolean();
    case KeyType::IntegerArray:
      return node.is_array() && all_elements(*node.as_array(), &toml::node::is_integer);
    case KeyType::StringArray:
      return node.is_array() && all_elements(*node.as_array(), &toml::node::is_string);
  }
  return false;
}

std::string_view article(KeyType type) {
  return type == KeyType::Integer || type == KeyType::IntegerArray || type == KeyType::StringArray
             ? "an"
             : "a";
}

std::string type_phrase(KeyType type) {
  switch (type) {
    case KeyType::IntegerArray: return "array of integers";
    case KeyType::StringArray: return "array of strings";
    default: return std::string(to_string(type));
  }
}

// "file:line: " for nodes parsed from a source, else "".
std::string where(const toml::source_region& src) {
  if (src.begin.line == 0) return {};
  std::string prefix = src.path ? *src.path : std::string();
  return prefix + ":" + std::to_string(src.begin.line) + ": ";
}

}  // namespace

std::string_view to_string(KeyType type) noexcept {
  switch (type) {
    case KeyType::String: return "string";
    case KeyType::Integer: return "integer";
    case KeyType::Float: return "float";
    case KeyType::Boolean: return "boolean";
    case KeyType::IntegerArray: return "array<integer>";
    case KeyType::StringArray: return "array<string>";
  }
  return "string";
}

DriverRegistry& DriverRegistry::global() {
  static DriverRegistry registry;
  return registry;
}

Result<void> DriverRegistry::add(std::string kind, DriverSchema schema, DriverFactory factory) {
  std::string problem;
  if (kind.empty()) {
    problem = "driver kind must not be empty";
  } else if (entries_.contains(kind)) {
    problem = "driver kind '" + kind + "' registered twice";
  } else if (!factory) {
    problem = "driver kind '" + kind + "' has no factory";
  }
  if (!problem.empty()) {
    Error error{ErrorKind::Config, problem, {}};
    conflicts_.push_back(error);
    return fail(std::move(error));
  }
  schema.kind = kind;
  entries_.emplace(std::move(kind), Entry{std::move(schema), std::move(factory)});
  return {};
}

bool DriverRegistry::contains(std::string_view kind) const { return entries_.find(kind) != entries_.end(); }

std::vector<std::string> DriverRegistry::kinds() const {
  std::vector<std::string> out;
  for (const auto& [kind, entry] : entries_) out.push_back(kind);
  return out;
}

const DriverSchema* DriverRegistry::schema(std::string_view kind) const {
  auto it = entries_.find(kind);
  return it == entries_.end() ? nullptr : &it->second.schema;
}

std::vector<DriverSchema> DriverRegistry::schemas() const {
  std::vector<DriverSchema> out;
  for (const auto& [kind, entry] : entries_) out.push_back(entry.schema);
  return out;
}

Result<void> DriverRegistry::validate(std::string_view kind, const toml::table& options) const {
  const DriverSchema* s = schema(kind);
  if (!s) return fail(ErrorKind::Config, "unknown driver kind '" + std::string(kind) + "'");

  std::vector<std::string> problems;
  for (const ConfigKey& key : s->keys) {
    const toml::node* node = options.get(key.name);
    if (!node) {
      if (key.required) problems.push_back(where(options.source()) + "missing required key '" + key.name + "'");
      continue;
    }
    if (!matches(*node, key.type)) {
      problems.push_back(where(node->source()) + "'" + key.name + "' must be " +
                         std::string(article(key.type)) + " " + type_phrase(key.type));
    }
  }
  for (const auto& [name, node] : options) {
    std::string_view key = name.str();
    if (is_common_key(key)) continue;
    bool declared = std::any_of(s->keys.begin(), s->keys.end(), [&](const ConfigKey& k) { return k.name == key; });
    if (!declared) {
      problems.push_back(where(node.source()) + "undeclared key '" + std::string(key) + "' for driver kind '" +
                         s->kind + "'");
    }
  }
  if (problems.empty()) return {};

  std::string what;
  for (const std::string& p : problems) {
    if (!what.empty()) what += "; ";
    what += p;
  }
  return fail(ErrorKind::Config, std::move(what));
}

Result<std::unique_ptr<Device>> DriverRegistry::create(std::string_view kind, Transport& transport,
                                                       const toml::table& options, DriverContext context) const {
  std::string name = context.name.empty() ? std::string(kind) : std::move(context.name);
  auto attributed = [&name](Error error) {
    if (error.device.empty()) error.device = name;
    return fail(std::move(error));
  };

  if (auto ok = validate(kind, options); !ok) return attributed(ok.error());

  const Entry& entry = entries_.find(kind)->second;
  auto made = entry.factory(DriverArgs{name, transport, options, context.clock});
  if (!made) return attributed(std::move(made).error());
  if (!*made) return attributed(Error{ErrorKind::Config, "driver factory returned no device", {}});
  return made;
}

Result<std::unique_ptr<Device>> DriverRegistry::create(const config::DriverConfig& config, Transport& transport,
                                                       const Clock* clock) const {
  return create(config.kind, transport, config.options, DriverContext{config.name, clock});
}

std::string describe(const DriverSchema& schema) {
  std::ostringstream out;
  out << schema.kind;
  if (!schema.summary.empty()) out << " - " << schema.summary;
  out << '\n';

  std::size_t name_w = 0;
  std::size_t type_w = 0;
  for (const ConfigKey& k : schema.keys) {
    name_w = std::max(name_w, k.name.size());
    type_w = std::max(type_w, to_string(k.type).size());
  }
  for (const ConfigKey& k : schema.keys) {
    std::string type(to_string(k.type));
    out << "  " << k.name << std::string(name_w - k.name.size() + 2, ' ') << type
        << std::string(type_w - type.size() + 2, ' ') << (k.required ? "required" : "optional");
    if (!k.description.empty()) out << "  " << k.description;
    out << '\n';
  }
  return out.str();
}

}  // namespace pychron
