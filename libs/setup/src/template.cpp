#include "pychron/setup/template.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <optional>

namespace pychron::setup {

namespace {

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

std::string format_double(double v) {
  if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  // Shortest form that reads back the same.
  for (int p = 1; p <= 17; ++p) {
    char shorter[32];
    std::snprintf(shorter, sizeof shorter, "%.*g", p, v);
    if (std::strtod(shorter, nullptr) == v) return shorter;
  }
  return buf;
}

std::string toml_string(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(c));
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

// --- parse tree ----------------------------------------------------------------

struct Node;
using Nodes = std::vector<std::unique_ptr<Node>>;

struct Node {
  enum class Kind { Text, Expr, If, For } kind = Kind::Text;
  int line = 0;
  std::string text;             // Text
  std::string path, filter;     // Expr; For: path is the list
  std::string condition;        // If
  std::string var;              // For
  Nodes body, otherwise;        // If / For
};

struct Token {
  enum class Kind { Text, Expr, Tag } kind;
  std::string text;
  int line;
};

Result<std::vector<Token>> tokenize(std::string_view src, std::string_view name) {
  // First drop lines that hold only a {% %} tag, keeping the tag.
  std::string text;
  {
    std::size_t pos = 0;
    while (pos <= src.size()) {
      const auto nl = src.find('\n', pos);
      const auto end = nl == std::string_view::npos ? src.size() : nl;
      const std::string_view line = src.substr(pos, end - pos);
      const std::string t = trim(line);
      const bool tag_only = t.size() >= 4 && t.starts_with("{%") && t.ends_with("%}") && t.find("{%", 2) == std::string::npos;
      if (tag_only) {
        text += t;
        text += '\x01';  // the line's end, to keep line numbers
      } else {
        text += line;
        if (nl != std::string_view::npos) text += '\n';
      }
      if (nl == std::string_view::npos) break;
      pos = nl + 1;
    }
  }
  std::vector<Token> out;
  int line = 1;
  std::size_t i = 0;
  std::string plain;
  auto flush = [&] {
    if (!plain.empty()) out.push_back({Token::Kind::Text, plain, line});
    plain.clear();
  };
  while (i < text.size()) {
    if (text.compare(i, 2, "{{") == 0 || text.compare(i, 2, "{%") == 0) {
      const bool expr = text[i + 1] == '{';
      const std::string close = expr ? "}}" : "%}";
      const auto end = text.find(close, i + 2);
      if (end == std::string::npos)
        return fail(ErrorKind::Config, std::string(name) + ":" + std::to_string(line) + ": unclosed " +
                                           (expr ? "{{" : "{%"));
      flush();
      out.push_back({expr ? Token::Kind::Expr : Token::Kind::Tag, trim(std::string_view(text).substr(i + 2, end - i - 2)), line});
      i = end + 2;
      continue;
    }
    if (text[i] == '\x01') {
      ++line;
      ++i;
      continue;
    }
    if (text[i] == '\n') ++line;
    plain += text[i++];
  }
  flush();
  return out;
}

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::string_view name) : tokens_(std::move(tokens)), name_(name) {}

  Result<Nodes> parse() {
    Nodes nodes;
    std::string stop;
    if (auto r = block(nodes, {}, stop); !r) return fail(r.error());
    return nodes;
  }

 private:
  Unexpected<Error> error(int line, const std::string& what) {
    return fail(ErrorKind::Config, name_ + ":" + std::to_string(line) + ": " + what);
  }

  // Parses until one of `stops` ("else", "endif", "endfor"); `stop` gets it.
  Result<void> block(Nodes& nodes, const std::vector<std::string>& stops, std::string& stop) {
    while (i_ < tokens_.size()) {
      const Token& t = tokens_[i_++];
      if (t.kind == Token::Kind::Text) {
        auto n = std::make_unique<Node>();
        n->text = t.text;
        n->line = t.line;
        nodes.push_back(std::move(n));
        continue;
      }
      if (t.kind == Token::Kind::Expr) {
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::Expr;
        n->line = t.line;
        const auto bar = t.text.find('|');
        n->path = trim(std::string_view(t.text).substr(0, bar));
        if (bar != std::string::npos) n->filter = trim(std::string_view(t.text).substr(bar + 1));
        if (n->path.empty()) return error(t.line, "empty {{ }}");
        if (!n->filter.empty() && n->filter != "toml") return error(t.line, "unknown filter '" + n->filter + "'");
        nodes.push_back(std::move(n));
        continue;
      }
      // Tag.
      const auto sp = t.text.find(' ');
      const std::string word = t.text.substr(0, sp);
      const std::string rest = sp == std::string::npos ? std::string{} : trim(std::string_view(t.text).substr(sp + 1));
      if (std::find(stops.begin(), stops.end(), word) != stops.end()) {
        stop = word;
        return {};
      }
      if (word == "if") {
        if (rest.empty()) return error(t.line, "{% if %} needs a condition");
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::If;
        n->line = t.line;
        n->condition = rest;
        std::string got;
        if (auto r = block(n->body, {"else", "endif"}, got); !r) return r;
        if (got.empty()) return error(t.line, "{% if %} without {% endif %}");
        if (got == "else") {
          if (auto r = block(n->otherwise, {"endif"}, got); !r) return r;
          if (got != "endif") return error(t.line, "{% else %} without {% endif %}");
        }
        nodes.push_back(std::move(n));
        continue;
      }
      if (word == "for") {
        const auto in = rest.find(" in ");
        if (in == std::string::npos) return error(t.line, "{% for x in list %} expected");
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::For;
        n->line = t.line;
        n->var = trim(std::string_view(rest).substr(0, in));
        n->path = trim(std::string_view(rest).substr(in + 4));
        std::string got;
        if (auto r = block(n->body, {"endfor"}, got); !r) return r;
        if (got != "endfor") return error(t.line, "{% for %} without {% endfor %}");
        nodes.push_back(std::move(n));
        continue;
      }
      return error(t.line, "unknown tag '" + word + "'");
    }
    stop.clear();
    return {};
  }

  std::vector<Token> tokens_;
  std::string name_;
  std::size_t i_ = 0;
};

// --- evaluation ----------------------------------------------------------------

struct Scope {
  const Answers& answers;
  std::map<std::string, Value> locals;  // loop variables
};

Result<Value> lookup(const Scope& scope, const std::string& path) {
  const auto dot = path.find('.');
  const std::string head = path.substr(0, dot);
  const Value* v = nullptr;
  if (auto it = scope.locals.find(head); it != scope.locals.end()) v = &it->second;
  else if (auto it2 = scope.answers.find(head); it2 != scope.answers.end()) v = &it2->second;
  if (v == nullptr) return fail(ErrorKind::Config, "unknown name '" + head + "'");
  if (dot == std::string::npos) return *v;
  // A row field: rows are stored as a one-row table.
  const auto* rows = std::get_if<std::vector<Row>>(v);
  if (rows == nullptr || rows->size() != 1) return fail(ErrorKind::Config, "'" + head + "' has no fields");
  const std::string field = path.substr(dot + 1);
  auto f = rows->front().find(field);
  if (f == rows->front().end()) return fail(ErrorKind::Config, "'" + head + "' has no field '" + field + "'");
  return Value{f->second};
}

Result<bool> eval_condition(std::string_view text, const Scope& scope) {
  std::string c = trim(text);
  bool negate = false;
  if (c.starts_with("not ")) {
    negate = true;
    c = trim(std::string_view(c).substr(4));
  }
  for (const std::string op : {"==", "!="}) {
    const auto at = c.find(op);
    if (at == std::string::npos) continue;
    const std::string lhs = trim(std::string_view(c).substr(0, at));
    std::string rhs = trim(std::string_view(c).substr(at + 2));
    if (rhs.size() < 2 || rhs.front() != '"' || rhs.back() != '"')
      return fail(ErrorKind::Config, "compare with a quoted string: " + std::string(text));
    rhs = rhs.substr(1, rhs.size() - 2);
    auto v = lookup(scope, lhs);
    if (!v) return fail(std::move(v).error());
    const bool equal = to_text(*v) == rhs;
    return negate != (op == "==" ? equal : !equal);
  }
  if (c.empty() || c.find(' ') != std::string::npos) return fail(ErrorKind::Config, "bad condition: " + std::string(text));
  auto v = lookup(scope, c);
  if (!v) return fail(std::move(v).error());
  return negate != truthy(*v);
}

Result<void> run(const Nodes& nodes, Scope& scope, std::string& out, const std::string& name) {
  for (const auto& n : nodes) {
    auto where = [&](const Error& e) {
      return fail(ErrorKind::Config, name + ":" + std::to_string(n->line) + ": " + e.what);
    };
    switch (n->kind) {
      case Node::Kind::Text: out += n->text; break;
      case Node::Kind::Expr: {
        auto v = lookup(scope, n->path);
        if (!v) return where(v.error());
        out += n->filter == "toml" ? to_toml(*v) : to_text(*v);
        break;
      }
      case Node::Kind::If: {
        auto c = eval_condition(n->condition, scope);
        if (!c) return where(c.error());
        if (auto r = run(*c ? n->body : n->otherwise, scope, out, name); !r) return r;
        break;
      }
      case Node::Kind::For: {
        auto v = lookup(scope, n->path);
        if (!v) return where(v.error());
        std::vector<Value> items;
        if (const auto* list = std::get_if<std::vector<std::string>>(&*v)) {
          for (const auto& s : *list) items.emplace_back(s);
        } else if (const auto* rows = std::get_if<std::vector<Row>>(&*v)) {
          for (const auto& r : *rows) items.emplace_back(std::vector<Row>{r});
        } else {
          return where(Error{ErrorKind::Config, "'" + n->path + "' is not a list", {}});
        }
        const auto saved = scope.locals.contains(n->var)
                               ? std::optional<Value>(scope.locals[n->var])
                               : std::nullopt;
        for (auto& item : items) {
          scope.locals[n->var] = std::move(item);
          if (auto r = run(n->body, scope, out, name); !r) return r;
        }
        if (saved) scope.locals[n->var] = *saved;
        else scope.locals.erase(n->var);
        break;
      }
    }
  }
  return {};
}

}  // namespace

std::string to_text(const Value& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(x);
        else if constexpr (std::is_same_v<T, double>) return format_double(x);
        else if constexpr (std::is_same_v<T, std::string>) return x;
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
          std::string out;
          for (const auto& s : x) out += (out.empty() ? "" : ", ") + s;
          return out;
        } else {
          return std::to_string(x.size());
        }
      },
      v);
}

std::string to_toml(const Value& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) return toml_string(x);
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
          std::string out = "[";
          for (std::size_t i = 0; i < x.size(); ++i) out += (i ? ", " : "") + toml_string(x[i]);
          return out + "]";
        } else if constexpr (std::is_same_v<T, std::vector<Row>>) {
          std::string out = "[";
          for (std::size_t i = 0; i < x.size(); ++i) {
            out += i ? ", { " : "{ ";
            bool first = true;
            for (const auto& [k, val] : x[i]) {
              out += (first ? "" : ", ") + k + " = " + toml_string(val);
              first = false;
            }
            out += " }";
          }
          return out + "]";
        } else {
          return to_text(Value{x});
        }
      },
      v);
}

bool truthy(const Value& v) {
  return std::visit(
      [](const auto& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) return x;
        else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) return x != 0;
        else return !x.empty();
      },
      v);
}

Result<std::string> render(std::string_view text, const Answers& answers, std::string_view name) {
  auto tokens = tokenize(text, name);
  if (!tokens) return fail(std::move(tokens).error());
  Parser parser(std::move(*tokens), name);
  auto nodes = parser.parse();
  if (!nodes) return fail(std::move(nodes).error());
  Scope scope{answers, {}};
  std::string out;
  if (auto r = run(*nodes, scope, out, std::string(name)); !r) return fail(std::move(r).error());
  return out;
}

Result<bool> evaluate(std::string_view condition, const Answers& answers) {
  Scope scope{answers, {}};
  return eval_condition(condition, scope);
}

}  // namespace pychron::setup
