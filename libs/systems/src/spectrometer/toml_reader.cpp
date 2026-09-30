#include "toml_reader.hpp"

#include <fstream>
#include <sstream>

namespace pychron::spectrometer::cfg::detail {

std::string_view type_name(toml::node_type t) {
  switch (t) {
    case toml::node_type::none: return "nothing";
    case toml::node_type::table: return "table";
    case toml::node_type::array: return "array";
    case toml::node_type::string: return "string";
    case toml::node_type::integer: return "integer";
    case toml::node_type::floating_point: return "float";
    case toml::node_type::boolean: return "boolean";
    case toml::node_type::date: return "date";
    case toml::node_type::time: return "time";
    case toml::node_type::date_time: return "date-time";
  }
  return "unknown";
}

SourceLoc Reader::loc(const toml::node& n) const {
  const auto& src = n.source();
  return SourceLoc{src.path ? std::string(*src.path) : file_, src.begin.line, src.begin.column};
}

void Reader::error(SourceLoc where, std::string field, std::string message) {
  out_.push_back({std::move(where), std::move(field), std::move(message)});
}

std::string Reader::join(const std::string& path, std::string_view key) {
  return path.empty() ? std::string(key) : path + "." + std::string(key);
}

const toml::table* Reader::table(const toml::node& n, const std::string& path) {
  if (const auto* t = n.as_table()) return t;
  error(loc(n), path, "expected table, got " + std::string(type_name(n.type())));
  return nullptr;
}

Obj Reader::obj(const toml::table& t, std::string path, Located* rec) {
  Obj o{t, std::move(path), loc(t), rec};
  // An empty inline table or the file root may carry no position.
  if (o.loc.line == 0) o.loc.file = file_;
  if (rec != nullptr) {
    rec->loc = o.loc;
    rec->path = o.path;
  }
  return o;
}

void Reader::only(const Obj& o, std::initializer_list<std::string_view> keys) {
  for (auto&& [k, v] : o.t) {
    bool known = false;
    for (auto key : keys) known = known || key == k.str();
    if (!known) error(loc(v), join(o.path, k.str()), "unknown field");
  }
}

const toml::node* Reader::get(const Obj& o, std::string_view key, bool required) {
  const auto* n = o.t.get(key);
  if (n == nullptr) {
    if (required) error(o.loc, join(o.path, key), "missing required field");
    return nullptr;
  }
  if (o.rec != nullptr) o.rec->field_locs[std::string(key)] = loc(*n);
  return n;
}

void Reader::type_error(const Obj& o, std::string_view key, const toml::node& n, std::string_view expected) {
  error(loc(n), join(o.path, key), "expected " + std::string(expected) + ", got " + std::string(type_name(n.type())));
}

std::optional<double> Reader::as_number(const toml::node& n) {
  if (const auto* f = n.as_floating_point()) return f->get();
  if (const auto* i = n.as_integer()) return static_cast<double>(i->get());
  return std::nullopt;
}

bool Reader::str(const Obj& o, std::string_view key, std::string& out, bool required) {
  const auto* n = get(o, key, required);
  if (n == nullptr) return !required;
  const auto* s = n->as_string();
  if (s == nullptr) {
    type_error(o, key, *n, "string");
    return false;
  }
  out = s->get();
  if (required && out.empty()) {
    error(loc(*n), join(o.path, key), "must not be empty");
    return false;
  }
  return true;
}

bool Reader::integer(const Obj& o, std::string_view key, std::int64_t& out, bool required, std::int64_t min) {
  const auto* n = get(o, key, required);
  if (n == nullptr) return !required;
  const auto* i = n->as_integer();
  if (i == nullptr) {
    type_error(o, key, *n, "integer");
    return false;
  }
  if (i->get() < min) {
    error(loc(*n), join(o.path, key),
          "value " + std::to_string(i->get()) + " out of range, must be >= " + std::to_string(min));
    return false;
  }
  out = i->get();
  return true;
}

bool Reader::number(const Obj& o, std::string_view key, double& out, bool required) {
  const auto* n = get(o, key, required);
  if (n == nullptr) return !required;
  auto v = as_number(*n);
  if (!v) {
    type_error(o, key, *n, "number");
    return false;
  }
  out = *v;
  return true;
}

bool Reader::number(const Obj& o, std::string_view key, std::optional<double>& out) {
  double v = 0.0;
  if (o.t.get(key) == nullptr) return true;
  if (!number(o, key, v, true)) return false;
  out = v;
  return true;
}

bool Reader::boolean(const Obj& o, std::string_view key, bool& out) {
  const auto* n = get(o, key, false);
  if (n == nullptr) return true;
  if (const auto* b = n->as_boolean()) {
    out = b->get();
    return true;
  }
  type_error(o, key, *n, "boolean");
  return false;
}

bool Reader::strings(const Obj& o, std::string_view key, std::vector<std::string>& out, bool required) {
  const auto* n = get(o, key, required);
  if (n == nullptr) return !required;
  const auto* arr = n->as_array();
  if (arr == nullptr) {
    type_error(o, key, *n, "array of strings");
    return false;
  }
  bool ok = true;
  out.clear();
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const auto& e = *arr->get(i);
    if (const auto* s = e.as_string()) {
      out.push_back(s->get());
    } else {
      error(loc(e), join(o.path, key) + "[" + std::to_string(i) + "]",
            "expected string, got " + std::string(type_name(e.type())));
      ok = false;
    }
  }
  return ok;
}

bool Reader::numbers(const Obj& o, std::string_view key, std::vector<double>& out) {
  const auto* n = get(o, key, false);
  if (n == nullptr) return true;
  const auto* arr = n->as_array();
  if (arr == nullptr) {
    type_error(o, key, *n, "array of numbers");
    return false;
  }
  bool ok = true;
  out.clear();
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const auto& e = *arr->get(i);
    if (auto v = as_number(e)) {
      out.push_back(*v);
    } else {
      error(loc(e), join(o.path, key) + "[" + std::to_string(i) + "]",
            "expected number, got " + std::string(type_name(e.type())));
      ok = false;
    }
  }
  return ok;
}

std::optional<Obj> Reader::sub(const Obj& o, std::string_view key, bool required, Located* rec) {
  const auto* n = get(o, key, required);
  if (n == nullptr) return std::nullopt;
  const auto path = join(o.path, key);
  const auto* t = table(*n, path);
  if (t == nullptr) return std::nullopt;
  Obj s = obj(*t, path, rec);
  if (s.loc.line == 0) s.loc = loc(*n);
  if (rec != nullptr) rec->loc = s.loc;
  return s;
}

void Reader::require(bool ok, const Obj& o, std::string_view key, std::string message) {
  if (ok) return;
  const auto* n = o.t.get(key);
  error(n != nullptr ? loc(*n) : o.loc, join(o.path, key), std::move(message));
}

std::optional<toml::table> parse_toml(std::string_view text, std::string_view name, std::vector<Diagnostic>& out) {
  auto result = toml::parse(text, name);
  if (!result) {
    const auto& err = result.error();
    const auto& src = err.source();
    out.push_back({SourceLoc{std::string(name), src.begin.line, src.begin.column}, "toml",
                   "syntax error: " + std::string(err.description())});
    return std::nullopt;
  }
  return std::move(result).table();
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace pychron::spectrometer::cfg::detail
