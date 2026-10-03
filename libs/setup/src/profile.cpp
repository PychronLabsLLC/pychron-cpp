#include "pychron/setup/profile.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

namespace pychron::setup {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const std::vector<std::pair<std::string_view, QuestionType>>& type_names() {
  static const std::vector<std::pair<std::string_view, QuestionType>> names{
      {"string", QuestionType::String}, {"host", QuestionType::Host},     {"port", QuestionType::Port},
      {"int", QuestionType::Int},       {"float", QuestionType::Float},   {"bool", QuestionType::Bool},
      {"choice", QuestionType::Choice}, {"path", QuestionType::Path},     {"folder", QuestionType::Folder},
      {"secret", QuestionType::Secret}, {"list", QuestionType::List},     {"table", QuestionType::Table}};
  return names;
}

// A TOML node as a Value; Rows from an array of tables.
std::optional<Value> value_of(const toml::node& n) {
  if (auto b = n.value<bool>(); b && n.is_boolean()) return Value{*b};
  if (n.is_integer()) return Value{*n.value<std::int64_t>()};
  if (n.is_floating_point()) return Value{*n.value<double>()};
  if (n.is_string()) return Value{*n.value<std::string>()};
  if (const auto* arr = n.as_array()) {
    if (arr->empty()) return Value{std::vector<std::string>{}};
    if (arr->is_array_of_tables()) {
      std::vector<Row> rows;
      for (const auto& e : *arr) {
        Row r;
        for (const auto& [k, v] : *e.as_table()) {
          if (auto s = v.value<std::string>()) r[std::string(k.str())] = *s;
          else if (auto any = value_of(v)) r[std::string(k.str())] = to_text(*any);
        }
        rows.push_back(std::move(r));
      }
      return Value{std::move(rows)};
    }
    std::vector<std::string> list;
    for (const auto& e : *arr) {
      auto s = e.value<std::string>();
      if (!s) return std::nullopt;
      list.push_back(*s);
    }
    return Value{std::move(list)};
  }
  return std::nullopt;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool valid_host(const std::string& s) {
  if (s.empty() || s.size() > 253) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '_';
  });
}

}  // namespace

std::string_view to_string(ProfileKind k) noexcept {
  switch (k) {
    case ProfileKind::Instrument: return "instrument";
    case ProfileKind::DataReduction: return "data_reduction";
    case ProfileKind::Fragment: return "fragment";
  }
  return "?";
}

std::string_view to_string(QuestionType t) noexcept {
  for (const auto& [name, type] : type_names())
    if (type == t) return name;
  return "?";
}

Result<Profile> load_profile(const fs::path& dir) {
  const fs::path manifest = dir / "profile.toml";
  const std::string where = manifest.string();
  auto parsed = toml::parse(read_file(manifest), where);
  if (!parsed) return fail(ErrorKind::Config, where + ": " + std::string(parsed.error().description()));
  const toml::table& t = parsed.table();
  std::vector<std::string> errors;
  auto err = [&](const std::string& field, const std::string& what) { errors.push_back(where + ": " + field + ": " + what); };

  Profile p;
  p.dir = dir;
  p.name = t["name"].value_or(std::string{});
  if (p.name.empty()) err("name", "missing");
  if (p.name != dir.filename().string()) err("name", "must match the directory name '" + dir.filename().string() + "'");
  p.title = t["title"].value_or(p.name);
  p.summary = t["summary"].value_or(std::string{});
  p.version = t["version"].value_or(std::int64_t{1});
  const std::string kind = t["kind"].value_or(std::string("fragment"));
  if (kind == "instrument") p.kind = ProfileKind::Instrument;
  else if (kind == "data_reduction") p.kind = ProfileKind::DataReduction;
  else if (kind == "fragment") p.kind = ProfileKind::Fragment;
  else err("kind", "must be instrument, data_reduction or fragment");
  if (const auto* g = t["groups"].as_array()) {
    for (const auto& e : *g) {
      if (auto s = e.value<std::string>()) p.groups.push_back(*s);
      else err("groups", "must be group names");
    }
  }
  if (const auto* inc = t["includes"].as_array()) {
    for (const auto& e : *inc) {
      if (auto s = e.value<std::string>()) p.includes.push_back(*s);
      else err("includes", "must be profile names");
    }
  }
  for (const auto& [k, v] : t) {
    static const std::set<std::string> known{"name",     "title",     "summary", "version", "kind",
                                             "includes", "questions", "files",   "values", "groups"};
    if (!known.contains(std::string(k.str()))) err(std::string(k.str()), "unknown key");
  }

  if (const auto* vals = t["values"].as_table()) {
    for (const auto& [k, v] : *vals) {
      if (auto value = value_of(v)) p.values[std::string(k.str())] = std::move(*value);
      else err("values." + std::string(k.str()), "unsupported value");
    }
  }
  if (const auto* qs = t["questions"].as_array()) {
    for (std::size_t i = 0; i < qs->size(); ++i) {
      const std::string w = "questions[" + std::to_string(i) + "]";
      const auto* q = (*qs)[i].as_table();
      if (q == nullptr) {
        err(w, "must be a table");
        continue;
      }
      Question question;
      question.profile = p.name;
      question.id = (*q)["id"].value_or(std::string{});
      if (question.id.empty()) err(w + ".id", "missing");
      question.prompt = (*q)["prompt"].value_or(question.id);
      question.help = (*q)["help"].value_or(std::string{});
      question.group = (*q)["group"].value_or(std::string("General"));
      question.when = (*q)["when"].value_or(std::string{});
      const std::string type = (*q)["type"].value_or(std::string("string"));
      auto it = std::find_if(type_names().begin(), type_names().end(), [&](const auto& e) { return e.first == type; });
      if (it == type_names().end()) err(w + ".type", "unknown type '" + type + "'");
      else question.type = it->second;
      if (const auto* d = (*q).get("default")) {
        if (auto v = value_of(*d)) question.default_value = *v;
        else err(w + ".default", "unsupported value");
      }
      if (const auto* c = (*q)["choices"].as_array())
        for (const auto& e : *c) question.choices.push_back(e.value_or(std::string{}));
      if (const auto* c = (*q)["labels"].as_array())
        for (const auto& e : *c) question.labels.push_back(e.value_or(std::string{}));
      if (!question.labels.empty() && question.labels.size() != question.choices.size())
        err(w + ".labels", "needs one label per choice");
      if (const auto* c = (*q)["columns"].as_array())
        for (const auto& e : *c) question.columns.push_back(e.value_or(std::string{}));
      if (question.type == QuestionType::Choice && question.choices.empty()) err(w + ".choices", "a choice needs choices");
      if (question.type == QuestionType::Table && question.columns.empty()) err(w + ".columns", "a table needs columns");
      if (question.type == QuestionType::Secret && question.default_value) err(w + ".default", "a secret has no default");
      p.questions.push_back(std::move(question));
    }
  }
  if (const auto* fsx = t["files"].as_array()) {
    for (std::size_t i = 0; i < fsx->size(); ++i) {
      const std::string w = "files[" + std::to_string(i) + "]";
      const auto* f = (*fsx)[i].as_table();
      if (f == nullptr) {
        err(w, "must be a table");
        continue;
      }
      FileSpec spec;
      spec.profile = p.name;
      spec.profile_dir = dir;
      spec.template_path = (*f)["template"].value_or(std::string{});
      spec.copy = (*f)["copy"].value_or(std::string{});
      spec.to = (*f)["to"].value_or(std::string{});
      spec.when = (*f)["when"].value_or(std::string{});
      spec.secret = (*f)["secret"].value_or(false);
      spec.check = (*f)["check"].value_or(std::string{});
      spec.convert = (*f)["convert"].value_or(std::string{});
      if (!spec.convert.empty() && spec.convert != "legacy_line" && spec.convert != "legacy_canvas")
        err(w + ".convert", "must be \"legacy_line\" or \"legacy_canvas\"");
      if (!spec.convert.empty() && spec.copy.empty()) err(w + ".convert", "needs copy naming the legacy folder");
      if (!spec.check.empty() && spec.check != "line" && spec.check != "canvas")
        err(w + ".check", "must be \"line\" or \"canvas\"");
      if (spec.template_path.empty() == spec.copy.empty()) err(w, "needs exactly one of template and copy");
      if (spec.to.empty()) err(w + ".to", "missing");
      if (fs::path(spec.to).is_absolute() || spec.to.find("..") != std::string::npos)
        err(w + ".to", "must stay inside the install root");
      if (!spec.template_path.empty() && !fs::exists(dir / spec.template_path))
        err(w + ".template", "no file " + (dir / spec.template_path).string());
      p.files.push_back(std::move(spec));
    }
  }
  if (!errors.empty()) {
    std::string all;
    for (const auto& e : errors) all += (all.empty() ? "" : "\n") + e;
    return fail(ErrorKind::Config, all);
  }
  return p;
}

Result<ProfileLibrary> ProfileLibrary::load(const fs::path& root, const fs::path& examples) {
  ProfileLibrary lib;
  lib.examples_ = examples;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return fail(ErrorKind::Config, "no profiles directory " + root.string());
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(root, ec))
    if (e.is_directory() && fs::exists(e.path() / "profile.toml")) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  std::string errors;
  for (const auto& d : dirs) {
    auto p = load_profile(d);
    if (!p) {
      errors += (errors.empty() ? "" : "\n") + p.error().what;
      continue;
    }
    lib.profiles_.emplace(p->name, std::move(*p));
  }
  if (!errors.empty()) return fail(ErrorKind::Config, errors);
  return lib;
}

std::vector<const Profile*> ProfileLibrary::list() const {
  std::vector<const Profile*> out;
  for (const auto& [name, p] : profiles_) out.push_back(&p);
  return out;
}

const Profile* ProfileLibrary::find(const std::string& name) const {
  auto it = profiles_.find(name);
  return it == profiles_.end() ? nullptr : &it->second;
}

Result<ResolvedProfile> ProfileLibrary::resolve(const std::string& name) const {
  const Profile* top = find(name);
  if (top == nullptr) return fail(ErrorKind::Config, "no profile '" + name + "'");
  ResolvedProfile out;
  out.top = *top;
  std::set<std::string> done, active;
  std::string error;
  std::function<void(const Profile&)> visit = [&](const Profile& p) {
    if (!error.empty() || done.contains(p.name)) return;
    if (active.contains(p.name)) {
      error = "profile '" + p.name + "' includes itself";
      return;
    }
    active.insert(p.name);
    for (const auto& inc : p.includes) {
      const Profile* q = find(inc);
      if (q == nullptr) {
        error = "profile '" + p.name + "' includes unknown profile '" + inc + "'";
        return;
      }
      visit(*q);
    }
    active.erase(p.name);
    done.insert(p.name);
    out.chain.push_back(p.name);
    out.versions[p.name] = p.version;
    for (const auto& [k, v] : p.values) out.values[k] = v;  // visited after its includes: wins
    for (const auto& q : p.questions) {
      auto same = std::find_if(out.questions.begin(), out.questions.end(), [&](const Question& x) { return x.id == q.id; });
      if (same == out.questions.end()) out.questions.push_back(q);
      else if (same->type != q.type)
        error = "question '" + q.id + "' is asked by '" + same->profile + "' and '" + q.profile + "' with different types";
    }
    for (const auto& f : p.files) out.files.push_back(f);
  };
  visit(*top);
  if (!error.empty()) return fail(ErrorKind::Config, error);
  std::map<std::string, std::string> owner;
  for (const auto& f : out.files) {
    // Conditional files may share a destination (one per branch).
    const std::string key = f.to + "|" + f.when;
    if (auto it = owner.find(key); it != owner.end() && it->second != f.profile)
      return fail(ErrorKind::Config, "'" + f.to + "' is written by both '" + it->second + "' and '" + f.profile + "'");
    owner[key] = f.profile;
  }
  // Hints from the top profile down to the deepest include, then the rest.
  for (auto it = out.chain.rbegin(); it != out.chain.rend(); ++it) {
    for (const auto& g : profiles_.at(*it).groups)
      if (std::find(out.groups.begin(), out.groups.end(), g) == out.groups.end()) out.groups.push_back(g);
  }
  for (const auto& q : out.questions)
    if (std::find(out.groups.begin(), out.groups.end(), q.group) == out.groups.end()) out.groups.push_back(q.group);
  return out;
}

// --- answers ----------------------------------------------------------------

Result<Value> parse_answer(const Question& q, std::string_view raw) {
  const std::string text = trim(raw);
  auto bad = [&](const std::string& what) {
    return fail(ErrorKind::Config, q.id + ": \"" + text + "\" " + what);
  };
  switch (q.type) {
    case QuestionType::String:
    case QuestionType::Path:
    case QuestionType::Folder:
      // A file is needed unless the question says what to use instead.
      if (text.empty() && !q.default_value)
        return fail(ErrorKind::Config, q.id + (q.type == QuestionType::Folder ? ": needs a folder" : ": needs a file"));
      return Value{text};
    case QuestionType::Secret: return Value{text};
    case QuestionType::Host:
      if (!valid_host(text)) return bad("is not a host name or address");
      return Value{text};
    case QuestionType::Port:
    case QuestionType::Int: {
      char* end = nullptr;
      const long long v = std::strtoll(text.c_str(), &end, 10);
      if (text.empty() || end == nullptr || *end != '\0') return bad("is not a whole number");
      if (q.type == QuestionType::Port && (v < 1 || v > 65535)) return bad("is not a port (1-65535)");
      return Value{static_cast<std::int64_t>(v)};
    }
    case QuestionType::Float: {
      char* end = nullptr;
      const double v = std::strtod(text.c_str(), &end);
      if (text.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) return bad("is not a number");
      return Value{v};
    }
    case QuestionType::Bool: {
      const std::string l = lower(text);
      if (l == "y" || l == "yes" || l == "true" || l == "1" || l == "on") return Value{true};
      if (l == "n" || l == "no" || l == "false" || l == "0" || l == "off") return Value{false};
      return bad("is not yes or no");
    }
    case QuestionType::Choice:
      if (std::find(q.choices.begin(), q.choices.end(), text) == q.choices.end()) {
        std::string all;
        for (const auto& c : q.choices) all += (all.empty() ? "" : ", ") + c;
        return bad("is not one of " + all);
      }
      return Value{text};
    case QuestionType::List: {
      std::vector<std::string> items;
      std::size_t pos = 0;
      while (pos <= text.size()) {
        const auto comma = text.find(',', pos);
        const std::string item = trim(std::string_view(text).substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (!item.empty()) items.push_back(item);
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      return Value{std::move(items)};
    }
    case QuestionType::Table: return bad("cannot be typed; give the table in an answers file");
  }
  return bad("is not understood");
}

namespace {

// A given value checked (and coerced from text) for `q`.
Result<Value> coerce(const Question& q, const Value& v) {
  if (const auto* s = std::get_if<std::string>(&v); s && q.type != QuestionType::Table) return parse_answer(q, *s);
  auto wrong = [&] {
    return fail(ErrorKind::Config, q.id + ": expected " + std::string(to_string(q.type)) + ", got " + to_text(v));
  };
  switch (q.type) {
    case QuestionType::Bool:
      if (std::holds_alternative<bool>(v)) return v;
      return wrong();
    case QuestionType::Int:
    case QuestionType::Port:
      if (const auto* i = std::get_if<std::int64_t>(&v)) {
        if (q.type == QuestionType::Port && (*i < 1 || *i > 65535)) return fail(ErrorKind::Config, q.id + ": not a port (1-65535)");
        return v;
      }
      return wrong();
    case QuestionType::Float:
      if (const auto* i = std::get_if<std::int64_t>(&v)) return Value{static_cast<double>(*i)};
      if (std::holds_alternative<double>(v)) return v;
      return wrong();
    case QuestionType::List:
      if (std::holds_alternative<std::vector<std::string>>(v)) return v;
      return wrong();
    case QuestionType::Table: {
      const auto* rows = std::get_if<std::vector<Row>>(&v);
      if (rows == nullptr) {
        if (const auto* l = std::get_if<std::vector<std::string>>(&v); l && l->empty()) return Value{std::vector<Row>{}};
        return wrong();
      }
      for (const auto& r : *rows)
        for (const auto& c : q.columns)
          if (!r.contains(c)) return fail(ErrorKind::Config, q.id + ": a row has no '" + c + "'");
      return v;
    }
    default: return wrong();
  }
}

}  // namespace

Result<Answers> complete_answers(const ResolvedProfile& profile, const Answers& given, const Answers& builtins) {
  Answers out = builtins;
  for (const auto& [k, v] : profile.values) out[k] = v;
  std::vector<std::string> errors;
  for (const auto& [id, v] : given) {
    if (std::none_of(profile.questions.begin(), profile.questions.end(), [&](const Question& q) { return q.id == id; }))
      errors.push_back(id + ": not a question of profile '" + profile.top.name + "'");
  }
  for (const auto& q : profile.questions) {
    bool asked = true;
    if (!q.when.empty()) {
      auto c = evaluate(q.when, out);
      if (!c) errors.push_back(q.id + ": when \"" + q.when + "\": " + c.error().what);
      asked = c && *c;
    }
    auto it = given.find(q.id);
    if (it != given.end()) {
      // Checked even when not asked, so a bad value is never silently ignored.
      auto v = coerce(q, it->second);
      if (!v) errors.push_back(v.error().what);
      else out[q.id] = std::move(*v);
    } else if (q.default_value) {
      out[q.id] = *q.default_value;  // unasked questions keep their default for templates
    } else if (asked) {
      errors.push_back(q.id + ": needs an answer (" + q.prompt + ")");
    }
  }
  if (!errors.empty()) {
    std::string all;
    for (const auto& e : errors) all += (all.empty() ? "" : "\n") + e;
    return fail(ErrorKind::Config, all);
  }
  return out;
}

Result<Answers> answers_from_toml(std::string_view text, std::string_view name) {
  auto parsed = toml::parse(text, name);
  if (!parsed) return fail(ErrorKind::Config, std::string(name) + ": " + std::string(parsed.error().description()));
  Answers out;
  for (const auto& [k, v] : parsed.table()) {
    auto value = value_of(v);
    if (!value) return fail(ErrorKind::Config, std::string(name) + ": " + std::string(k.str()) + ": unsupported value");
    out[std::string(k.str())] = std::move(*value);
  }
  return out;
}

Result<std::pair<std::string, std::string>> parse_assignment(std::string_view text) {
  const auto eq = text.find('=');
  if (eq == std::string_view::npos || eq == 0) return fail(ErrorKind::Config, "expected id=value, got \"" + std::string(text) + "\"");
  return std::pair{trim(text.substr(0, eq)), trim(text.substr(eq + 1))};
}

}  // namespace pychron::setup
