#include "pychron/processing/options.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "pychron/processing/quantity.hpp"

namespace pychron::processing {

namespace fs = std::filesystem;

namespace {

Unexpected<Error> opt_fail(std::string what) { return fail(ErrorKind::Config, "options: " + std::move(what)); }

std::string format_double(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

std::string value_text(const OptionValue& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "<unset>";
        if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(x);
        if constexpr (std::is_same_v<T, double>) return format_double(x);
        if constexpr (std::is_same_v<T, std::string>) return '"' + x + '"';
        if constexpr (std::is_same_v<T, std::vector<std::string>>) {
          std::string s = "[";
          for (std::size_t i = 0; i < x.size(); ++i) s += (i ? ",\"" : "\"") + x[i] + '"';
          return s + "]";
        }
      },
      v);
}

constexpr std::string_view kPaletteNames[] = {"black", "white", "red",  "green",  "blue",   "orange", "purple",
                                              "brown", "gray",  "grey", "cyan",   "magenta", "yellow", "navy",
                                              "teal",  "olive", "maroon", "pink", "lime",   "gold"};

}  // namespace

std::string_view to_string(FieldType type) noexcept {
  switch (type) {
    case FieldType::Bool:
      return "bool";
    case FieldType::Int:
      return "int";
    case FieldType::Double:
      return "double";
    case FieldType::String:
      return "string";
    case FieldType::Enum:
      return "enum";
    case FieldType::Color:
      return "color";
    case FieldType::Quantity:
      return "quantity";
    case FieldType::StringList:
      return "string_list";
    case FieldType::Font:
      return "font";
  }
  return "string";
}

bool is_color(std::string_view t) {
  if (!t.empty() && t[0] == '#') {
    if (t.size() != 7 && t.size() != 9) return false;
    return std::all_of(t.begin() + 1, t.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
  }
  return std::find(std::begin(kPaletteNames), std::end(kPaletteNames), t) != std::end(kPaletteNames);
}

const FieldSpec* Schema::field(std::string_view key) const {
  for (const auto& f : fields)
    if (f.key == key) return &f;
  return nullptr;
}

const ListSpec* Schema::list(std::string_view key) const {
  for (const auto& l : lists)
    if (l.key == key) return &l;
  return nullptr;
}

Result<OptionValue> validate(const FieldSpec& f, const OptionValue& v) {
  const std::string where = "'" + f.key + "': ";
  if (std::holds_alternative<std::monostate>(v)) {
    if (f.optional) return v;
    return opt_fail(where + "a value is required");
  }
  switch (f.type) {
    case FieldType::Bool:
      if (std::holds_alternative<bool>(v)) return v;
      return opt_fail(where + "expected true or false");
    case FieldType::Int: {
      std::int64_t i = 0;
      if (const auto* p = std::get_if<std::int64_t>(&v)) {
        i = *p;
      } else if (const auto* d = std::get_if<double>(&v); d && std::isfinite(*d) && std::floor(*d) == *d) {
        i = static_cast<std::int64_t>(*d);
      } else {
        return opt_fail(where + "expected an integer");
      }
      if (static_cast<double>(i) < f.min || static_cast<double>(i) > f.max)
        return opt_fail(where + std::to_string(i) + " is out of range");
      return OptionValue(i);
    }
    case FieldType::Double: {
      double d = 0;
      if (const auto* p = std::get_if<double>(&v))
        d = *p;
      else if (const auto* i = std::get_if<std::int64_t>(&v))
        d = static_cast<double>(*i);
      else
        return opt_fail(where + "expected a number");
      if (!std::isfinite(d)) return opt_fail(where + "expected a finite number");
      if (d < f.min || d > f.max) return opt_fail(where + format_double(d) + " is out of range");
      return OptionValue(d);
    }
    case FieldType::String:
      if (std::holds_alternative<std::string>(v)) return v;
      return opt_fail(where + "expected text");
    case FieldType::Font:
      // Any name: which families are installed is the display's business.
      if (std::holds_alternative<std::string>(v)) return v;
      return opt_fail(where + "expected a font family");
    case FieldType::Enum: {
      const auto* s = std::get_if<std::string>(&v);
      if (!s) return opt_fail(where + "expected one of the choices");
      if (std::find(f.choices.begin(), f.choices.end(), *s) == f.choices.end())
        return opt_fail(where + "'" + *s + "' is not a choice");
      return v;
    }
    case FieldType::Color: {
      const auto* s = std::get_if<std::string>(&v);
      if (!s || !is_color(*s)) return opt_fail(where + "expected a colour (#rrggbb, #rrggbbaa or a name)");
      return v;
    }
    case FieldType::Quantity: {
      const auto* s = std::get_if<std::string>(&v);
      if (!s) return opt_fail(where + "expected a quantity");
      if (s->empty() && f.optional) return v;
      auto q = Quantity::parse(*s);
      if (!q) return opt_fail(where + q.error().what);
      return OptionValue(q->text());
    }
    case FieldType::StringList:
      if (std::holds_alternative<std::vector<std::string>>(v)) return v;
      if (const auto* s = std::get_if<std::string>(&v)) return OptionValue(std::vector<std::string>{*s});
      return opt_fail(where + "expected a list of text");
  }
  return v;
}

// ---------------------------------------------------------------- Options

OptionValue Options::get(std::string_view key) const {
  auto it = values_.find(std::string(key));
  if (it != values_.end()) return it->second;
  const FieldSpec* f = schema_ ? schema_->field(key) : nullptr;
  assert(f != nullptr && "Options::get: key not in schema");
  if (!f) return std::monostate{};
  return f->default_value;
}

bool Options::is_set(std::string_view key) const { return values_.count(std::string(key)) != 0; }

bool Options::get_bool(std::string_view key) const {
  const auto v = get(key);
  if (const auto* b = std::get_if<bool>(&v)) return *b;
  return false;
}

std::int64_t Options::get_int(std::string_view key) const {
  const auto v = get(key);
  if (const auto* i = std::get_if<std::int64_t>(&v)) return *i;
  if (const auto* d = std::get_if<double>(&v)) return static_cast<std::int64_t>(std::llround(*d));
  return 0;
}

double Options::get_double(std::string_view key) const { return get_optional_double(key).value_or(0.0); }

std::optional<double> Options::get_optional_double(std::string_view key) const {
  const auto v = get(key);
  if (const auto* d = std::get_if<double>(&v)) return *d;
  if (const auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
  return std::nullopt;
}

std::string Options::get_string(std::string_view key) const {
  const auto v = get(key);
  if (const auto* s = std::get_if<std::string>(&v)) return *s;
  return {};
}

std::vector<std::string> Options::get_strings(std::string_view key) const {
  const auto v = get(key);
  if (const auto* s = std::get_if<std::vector<std::string>>(&v)) return *s;
  if (const auto* s = std::get_if<std::string>(&v)) return {*s};
  return {};
}

Result<void> Options::set(std::string_view key, OptionValue value) {
  const FieldSpec* f = schema_ ? schema_->field(key) : nullptr;
  if (!f) return opt_fail("unknown key '" + std::string(key) + "'");
  auto ok = validate(*f, value);
  if (!ok) return fail(ok.error());
  if (std::holds_alternative<std::monostate>(*ok) && std::holds_alternative<std::monostate>(f->default_value)) {
    values_.erase(std::string(key));
  } else {
    values_[std::string(key)] = std::move(*ok);
  }
  return {};
}

void Options::unset(std::string_view key) { values_.erase(std::string(key)); }

std::vector<Options> Options::rows(std::string_view key) const {
  auto it = lists_.find(std::string(key));
  if (it != lists_.end()) return it->second;
  const ListSpec* l = schema_ ? schema_->list(key) : nullptr;
  assert(l != nullptr && "Options::rows: list not in schema");
  if (!l) return {};
  std::vector<Options> out;
  for (const auto& text : l->default_rows_toml) {
    auto loaded = options_from_toml(l->row, text);
    out.push_back(loaded ? std::move(loaded->options) : Options(l->row));
  }
  return out;
}

Result<void> Options::set_rows(std::string_view key, std::vector<Options> rows) {
  const ListSpec* l = schema_ ? schema_->list(key) : nullptr;
  if (!l) return opt_fail("unknown list '" + std::string(key) + "'");
  if (rows.size() < l->min_rows || rows.size() > l->max_rows)
    return opt_fail("'" + l->key + "': " + std::to_string(rows.size()) + " rows is out of range");
  for (auto& r : rows)
    if (r.schema() != l->row) return opt_fail("'" + l->key + "': row has the wrong schema");
  lists_[std::string(key)] = std::move(rows);
  return {};
}

Options Options::new_row(std::string_view list_key) const {
  const ListSpec* l = schema_ ? schema_->list(list_key) : nullptr;
  return Options(l ? l->row : nullptr);
}

std::string Options::canonical() const {
  std::string out;
  for (const auto& [k, v] : values_) out += k + "=" + value_text(v) + "\n";
  for (const auto& [k, v] : extra) out += "?" + k + "=" + value_text(v) + "\n";
  auto rows_text = [&](const std::string& prefix, const std::string& k, const std::vector<Options>& rows) {
    out += prefix + k + "#" + std::to_string(rows.size()) + "\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      std::istringstream in(rows[i].canonical());
      std::string line;
      while (std::getline(in, line)) out += prefix + k + "[" + std::to_string(i) + "]." + line + "\n";
    }
  };
  for (const auto& [k, rows] : lists_) rows_text("", k, rows);
  for (const auto& [k, rows] : extra_lists) rows_text("?", k, rows);
  return out;
}

// ---------------------------------------------------------------- TOML

namespace {

std::optional<OptionValue> from_node(const toml::node& n) {
  if (auto b = n.value_exact<bool>()) return OptionValue(*b);
  if (auto i = n.value_exact<std::int64_t>()) return OptionValue(*i);
  if (auto d = n.value_exact<double>()) return OptionValue(*d);
  if (auto s = n.value_exact<std::string>()) return OptionValue(*s);
  if (const auto* a = n.as_array()) {
    std::vector<std::string> out;
    for (const auto& e : *a) {
      auto s = e.value_exact<std::string>();
      if (!s) return std::nullopt;
      out.push_back(*s);
    }
    return OptionValue(std::move(out));
  }
  return std::nullopt;
}

void read_table(const toml::table& t, const std::string& prefix, Options& out, std::vector<std::string>& warnings,
                bool top);

void read_rows(const toml::array& a, const std::string& key, const SchemaPtr& row_schema, Options& out,
               std::vector<std::string>& warnings, bool known) {
  std::vector<Options> rows;
  for (const auto& e : a) {
    const auto* t = e.as_table();
    if (!t) {
      warnings.push_back("'" + key + "': expected a table per row; ignored");
      continue;
    }
    Options row(row_schema);
    read_table(*t, "", row, warnings, false);
    rows.push_back(std::move(row));
  }
  if (known)
    out.raw_lists()[key] = std::move(rows);
  else
    out.extra_lists[key] = std::move(rows);
}

void read_table(const toml::table& t, const std::string& prefix, Options& out, std::vector<std::string>& warnings,
                bool top) {
  const Schema* schema = out.schema().get();
  for (const auto& [k, node] : t) {
    const std::string key = prefix + std::string(k.str());
    if (top && prefix.empty() && (key == "schema" || key == "version" || key == "name")) continue;
    if (const auto* sub = node.as_table()) {
      read_table(*sub, key + ".", out, warnings, top);
      continue;
    }
    if (const auto* arr = node.as_array(); arr && !arr->empty() && arr->front().is_table()) {
      const ListSpec* l = schema ? schema->list(key) : nullptr;
      read_rows(*arr, key, l ? l->row : nullptr, out, warnings, l != nullptr);
      continue;
    }
    auto v = from_node(node);
    if (!v) {
      warnings.push_back("'" + key + "': unsupported value; ignored");
      continue;
    }
    if (schema && schema->field(key)) {
      out.raw_values()[key] = std::move(*v);
    } else {
      out.extra[key] = std::move(*v);
    }
  }
}

void validate_all(Options& o, std::vector<std::string>& warnings) {
  if (!o.schema()) return;
  for (auto it = o.raw_values().begin(); it != o.raw_values().end();) {
    const FieldSpec* f = o.schema()->field(it->first);
    if (!f) {
      o.extra[it->first] = it->second;
      it = o.raw_values().erase(it);
      continue;
    }
    auto ok = validate(*f, it->second);
    if (!ok) {
      warnings.push_back(ok.error().what + "; using the default");
      it = o.raw_values().erase(it);
      continue;
    }
    it->second = std::move(*ok);
    ++it;
  }
  for (auto& [k, rows] : o.raw_lists())
    for (auto& r : rows) validate_all(r, warnings);
}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): what it holds is moved out, by kind, below
void put(toml::table& root, const std::string& dotted, toml::node&& node_value) {
  toml::table* t = &root;
  std::size_t start = 0;
  while (true) {
    const auto dot = dotted.find('.', start);
    if (dot == std::string::npos) break;
    const std::string part = dotted.substr(start, dot - start);
    auto* existing = t->get(part);
    if (!existing || !existing->is_table()) {
      t->insert_or_assign(part, toml::table{});
      existing = t->get(part);
    }
    t = existing->as_table();
    start = dot + 1;
  }
  const std::string last = dotted.substr(start);
  if (auto* tab = node_value.as_table())
    t->insert_or_assign(last, std::move(*tab));
  else if (auto* arr = node_value.as_array())
    t->insert_or_assign(last, std::move(*arr));
  else if (auto b = node_value.value_exact<bool>())
    t->insert_or_assign(last, *b);
  else if (auto i = node_value.value_exact<std::int64_t>())
    t->insert_or_assign(last, *i);
  else if (auto d = node_value.value_exact<double>())
    t->insert_or_assign(last, *d);
  else if (auto s = node_value.value_exact<std::string>())
    t->insert_or_assign(last, *s);
}

void put_value(toml::table& root, const std::string& key, const OptionValue& v) {
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
          toml::array a;
          for (const auto& s : x) a.push_back(s);
          put(root, key, std::move(a));
        } else {
          put(root, key, toml::value<T>(x));
        }
      },
      v);
}

toml::table to_table(const Options& o) {
  toml::table root;
  for (const auto& [k, v] : o.raw_values()) put_value(root, k, v);
  for (const auto& [k, v] : o.extra) put_value(root, k, v);
  auto rows_out = [&](const std::string& k, const std::vector<Options>& rows) {
    toml::array a;
    for (const auto& r : rows) a.push_back(to_table(r));
    put(root, k, std::move(a));
  };
  for (const auto& [k, rows] : o.raw_lists()) rows_out(k, rows);
  for (const auto& [k, rows] : o.extra_lists) rows_out(k, rows);
  return root;
}

}  // namespace

Result<LoadedOptions> options_from_toml(const SchemaPtr& schema, std::string_view text) {
  auto parsed = toml::parse(text);
  if (!parsed) return opt_fail(std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();
  LoadedOptions out;
  out.options = Options(schema);
  if (const auto* s = root.get("schema")) {
    const auto kind = s->value_exact<std::string>();
    if (!kind || !schema || *kind != schema->kind)
      return opt_fail("document is for '" + kind.value_or("?") + "', expected '" + (schema ? schema->kind : "") + "'");
  }
  if (const auto* n = root.get("name")) out.name = n->value_exact<std::string>().value_or("");
  const int current = schema ? schema->version : 1;
  out.file_version = current;
  if (const auto* v = root.get("version")) out.file_version = static_cast<int>(v->value_exact<std::int64_t>().value_or(current));
  read_table(root, "", out.options, out.warnings, true);
  if (schema) {
    for (int v = std::max(1, out.file_version); v < current; ++v)
      if (static_cast<std::size_t>(v - 1) < schema->migrations.size() && schema->migrations[v - 1])
        schema->migrations[v - 1](out.options);
    if (out.file_version > current)
      out.warnings.push_back("written by a newer version (" + std::to_string(out.file_version) +
                             "); unknown settings are kept but not used");
  }
  validate_all(out.options, out.warnings);
  return out;
}

std::string options_to_toml(const Options& options, std::string_view name) {
  toml::table body = to_table(options);
  std::ostringstream os;
  if (options.schema()) {
    os << "schema = \"" << options.schema()->kind << "\"\n";
    os << "version = " << options.schema()->version << "\n";
  }
  if (!name.empty()) {
    toml::table n;
    n.insert("name", std::string(name));
    os << n << "\n";
  }
  os << "\n" << body << "\n";
  return os.str();
}

// ---------------------------------------------------------------- presets

PresetStore::PresetStore(fs::path user_root, std::optional<fs::path> lab_root)
    : user_root_(std::move(user_root)), lab_root_(std::move(lab_root)) {}

fs::path PresetStore::dir(const fs::path& root, const Schema& schema) const { return root / schema.kind; }

std::string PresetStore::file_stem(std::string_view name) {
  std::string out;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    out += std::isalnum(u) ? static_cast<char>(std::tolower(u)) : '_';
  }
  return out;
}

Result<void> PresetStore::check_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return opt_fail("a preset name must have 1 to 64 characters");
  if (name.find_first_of("/\\") != std::string_view::npos) return opt_fail("a preset name cannot contain / or \\");
  if (std::all_of(name.begin(), name.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }))
    return opt_fail("a preset name cannot be blank");
  return {};
}

namespace {

Result<std::string> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string name_in_file(const fs::path& p) {
  auto text = read_file(p);
  if (!text) return p.stem().string();
  auto parsed = toml::parse(*text);
  if (!parsed) return p.stem().string();
  const toml::table root = std::move(parsed).table();
  if (const auto* n = root.get("name"))
    if (auto s = n->value_exact<std::string>(); s && !s->empty()) return *s;
  return p.stem().string();
}

}  // namespace

std::vector<PresetInfo> PresetStore::list(const SchemaPtr& schema) const {
  std::map<std::string, PresetInfo> by_stem;
  if (!schema) return {};
  for (const auto& [name, _] : schema->factory_presets)
    by_stem[file_stem(name)] = PresetInfo{name, PresetOrigin::Factory, {}, false};
  auto scan = [&](const fs::path& root, PresetOrigin origin) {
    std::error_code ec;
    const fs::path d = dir(root, *schema);
    if (!fs::is_directory(d, ec)) return;
    for (const auto& e : fs::directory_iterator(d, ec)) {
      if (!e.is_regular_file() || e.path().extension() != ".toml") continue;
      const std::string stem = e.path().stem().string();
      const bool shadows = by_stem.count(stem) != 0;
      by_stem[stem] = PresetInfo{name_in_file(e.path()), origin, e.path(), shadows};
    }
  };
  if (lab_root_) scan(*lab_root_, PresetOrigin::Lab);
  scan(user_root_, PresetOrigin::User);
  std::vector<PresetInfo> out;
  for (auto& [_, info] : by_stem) out.push_back(std::move(info));
  std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });
  return out;
}

Result<LoadedOptions> PresetStore::factory(const SchemaPtr& schema, std::string_view name) const {
  if (!schema) return opt_fail("no schema");
  for (const auto& [n, text] : schema->factory_presets) {
    if (file_stem(n) != file_stem(name)) continue;
    auto loaded = options_from_toml(schema, text);
    if (!loaded) return fail(loaded.error());
    loaded->name = n;
    return loaded;
  }
  return opt_fail("no factory preset '" + std::string(name) + "' for " + schema->kind);
}

Options PresetStore::defaults(const SchemaPtr& schema) const {
  if (schema && !schema->factory_presets.empty()) {
    auto f = factory(schema, schema->factory_presets.front().first);
    if (f) return f->options;
  }
  return Options(schema);
}

Result<LoadedOptions> PresetStore::load(const SchemaPtr& schema, std::string_view name) const {
  if (!schema) return opt_fail("no schema");
  const std::string stem = file_stem(name);
  for (const auto& root : {std::optional<fs::path>(user_root_), lab_root_}) {
    if (!root) continue;
    const fs::path p = dir(*root, *schema) / (stem + ".toml");
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) continue;
    auto text = read_file(p);
    if (!text) return fail(text.error());
    auto loaded = options_from_toml(schema, *text);
    if (!loaded) return fail(ErrorKind::Config, p.string() + ": " + loaded.error().what);
    if (loaded->name.empty()) loaded->name = std::string(name);
    return loaded;
  }
  return factory(schema, name);
}

Result<void> PresetStore::save(std::string_view name, const Options& options) const {
  if (auto ok = check_name(name); !ok) return ok;
  if (!options.schema()) return opt_fail("options without a schema cannot be saved");
  const fs::path d = dir(user_root_, *options.schema());
  std::error_code ec;
  fs::create_directories(d, ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + d.string() + ": " + ec.message());
  const fs::path p = d / (file_stem(name) + ".toml");
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail(ErrorKind::Io, "cannot write " + tmp.string());
    out << options_to_toml(options, name);
    if (!out) return fail(ErrorKind::Io, "cannot write " + tmp.string());
  }
  fs::rename(tmp, p, ec);
  if (ec) return fail(ErrorKind::Io, "cannot write " + p.string() + ": " + ec.message());
  return {};
}

Result<void> PresetStore::remove(const SchemaPtr& schema, std::string_view name) const {
  if (!schema) return opt_fail("no schema");
  const fs::path p = dir(user_root_, *schema) / (file_stem(name) + ".toml");
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return opt_fail("'" + std::string(name) + "' is not a user preset");
  fs::remove(p, ec);
  if (ec) return fail(ErrorKind::Io, "cannot delete " + p.string() + ": " + ec.message());
  return {};
}

Result<void> PresetStore::rename(const SchemaPtr& schema, std::string_view from, std::string_view to) const {
  if (auto ok = check_name(to); !ok) return ok;
  if (!schema) return opt_fail("no schema");
  std::error_code ec;
  const fs::path src = dir(user_root_, *schema) / (file_stem(from) + ".toml");
  if (!fs::is_regular_file(src, ec)) return opt_fail("'" + std::string(from) + "' is not a user preset");
  const fs::path dst = dir(user_root_, *schema) / (file_stem(to) + ".toml");
  if (file_stem(from) != file_stem(to) && fs::exists(dst, ec))
    return opt_fail("a user preset named '" + std::string(to) + "' exists");
  auto loaded = load(schema, from);
  if (!loaded) return fail(loaded.error());
  if (file_stem(from) != file_stem(to)) fs::remove(src, ec);
  return save(to, loaded->options);
}

}  // namespace pychron::processing
