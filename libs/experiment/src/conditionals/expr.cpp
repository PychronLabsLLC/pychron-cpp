#include "pychron/experiment/conditionals/expr.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pychron::experiment {
namespace {

// Accepted isotope fields and the canonical name each maps to.
struct FieldAlias {
  std::string_view text, canonical;
};
constexpr FieldAlias kIsoFields[] = {{"cur", "cur"},
                                     {"current", "cur"},
                                     {"bs", "bs"},
                                     {"bs_corrected", "bs_corrected"},
                                     {"ic_corrected", "ic_corrected"},
                                     {"intercept", "intercept"},
                                     {"std_dev", "std_dev"},
                                     {"sd", "std_dev"},
                                     {"stddev", "std_dev"}};
constexpr std::string_view kDetFields[] = {"deflection", "inactive", "intensity"};
constexpr std::string_view kComputed[] = {"age",   "instant_age", "kca",  "cak",  "kcl",  "clk",  "radiogenic_yield",
                                          "rad40", "rad40_percent", "atm40", "k39", "ca37", "ca39", "ca36", "cl36"};

std::optional<std::string_view> iso_field(std::string_view s) {
  for (const auto& f : kIsoFields)
    if (f.text == s) return f.canonical;
  return std::nullopt;
}

bool det_field(std::string_view s) { return std::find(std::begin(kDetFields), std::end(kDetFields), s) != std::end(kDetFields); }
bool computed(std::string_view s) { return std::find(std::begin(kComputed), std::end(kComputed), s) != std::end(kComputed); }

struct FuncName {
  std::string_view name;
  Func func;
};
constexpr FuncName kFuncs[] = {{"min", Func::Min},         {"max", Func::Max},     {"average", Func::Average},
                               {"slope", Func::Slope},     {"std", Func::Std},     {"rsd", Func::Rsd},
                               {"between", Func::Between}, {"count", Func::Count}, {"elapsed", Func::Elapsed},
                               {"abs", Func::Abs}};

std::optional<Func> find_func(std::string_view s) {
  for (const auto& f : kFuncs)
    if (f.name == s) return f.func;
  return std::nullopt;
}

bool is_series_func(Func f) {
  return f == Func::Min || f == Func::Max || f == Func::Average || f == Func::Slope || f == Func::Std ||
         f == Func::Rsd || f == Func::Count;
}

bool is_keyword(std::string_view s) { return s == "and" || s == "or" || s == "not"; }

enum class Tok { End, Num, Ident, Var, LParen, RParen, Comma, Slash, Star, Plus, Minus, Assign, Cmp };
struct Token {
  Tok t = Tok::End;
  std::string text;
  double num = 0;
  CmpOp op = CmpOp::Lt;
  size_t pos = 0;
};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Length of "(NAME)" at s[i] (a detector qualifier as in "L2(CDD)"), or 0.
size_t paren_name(std::string_view s, size_t i) {
  if (i >= s.size() || s[i] != '(' || i + 1 >= s.size() || !ident_start(s[i + 1])) return 0;
  size_t j = i + 1;
  while (j < s.size() && ident_char(s[j])) ++j;
  return j < s.size() && s[j] == ')' ? j + 1 - i : 0;
}

Result<std::vector<Token>> tokenize(std::string_view s) {
  std::vector<Token> out;
  size_t i = 0;
  auto err = [&](const std::string& m) { return fail(ErrorKind::Config, "conditional '" + std::string(s) + "': " + m); };
  while (i < s.size()) {
    char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    Token t;
    t.pos = i;
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      size_t j = i;
      while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
      if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
        if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
          while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
          j = k;
        }
      }
      t.t = Tok::Num;
      t.text = std::string(s.substr(i, j - i));
      char* end = nullptr;
      t.num = std::strtod(t.text.c_str(), &end);
      if (end != t.text.c_str() + t.text.size()) return err("bad number '" + t.text + "'");
      i = j;
    } else if (ident_start(c)) {
      size_t j = i;
      while (j < s.size() && ident_char(s[j])) ++j;
      // "L2(CDD)": a detector qualifier, unless the name is a function.
      if (!find_func(s.substr(i, j - i))) j += paren_name(s, j);
      while (j + 1 < s.size() && s[j] == '.' && ident_start(s[j + 1])) {
        ++j;
        while (j < s.size() && ident_char(s[j])) ++j;
      }
      t.t = Tok::Ident;
      t.text = std::string(s.substr(i, j - i));
      i = j;
    } else if (c == '$') {
      size_t j = i + 1;
      while (j < s.size() && ident_char(s[j])) ++j;
      if (j == i + 1) return err("expected a name after '$'");
      t.t = Tok::Var;
      t.text = std::string(s.substr(i + 1, j - i - 1));
      i = j;
    } else if (c == '<' || c == '>' || c == '=' || c == '!') {
      bool eq = i + 1 < s.size() && s[i + 1] == '=';
      t.t = Tok::Cmp;
      if (c == '<') t.op = eq ? CmpOp::Le : CmpOp::Lt;
      else if (c == '>') t.op = eq ? CmpOp::Ge : CmpOp::Gt;
      else if (c == '=' && eq) t.op = CmpOp::Eq;
      else if (c == '!' && eq) t.op = CmpOp::Ne;
      else if (c == '=') {
        t.t = Tok::Assign;
        i += 1;
        out.push_back(t);
        continue;
      } else {
        return err("unexpected '!' at " + std::to_string(i));
      }
      i += eq ? 2 : 1;
    } else {
      switch (c) {
        case '(': t.t = Tok::LParen; break;
        case ')': t.t = Tok::RParen; break;
        case ',': t.t = Tok::Comma; break;
        case '/': t.t = Tok::Slash; break;
        case '*': t.t = Tok::Star; break;
        case '+': t.t = Tok::Plus; break;
        case '-': t.t = Tok::Minus; break;
        default: return err(std::string("unexpected '") + c + "' at " + std::to_string(i));
      }
      ++i;
    }
    out.push_back(std::move(t));
  }
  out.push_back(Token{Tok::End, "", 0, CmpOp::Lt, s.size()});
  return out;
}

ExprPtr make(Expr::Kind k) {
  auto e = std::make_unique<Expr>();
  e->kind = k;
  return e;
}

ExprPtr binary(Expr::Kind k, ExprPtr l, ExprPtr r) {
  auto n = make(k);
  n->children.push_back(std::move(l));
  n->children.push_back(std::move(r));
  return n;
}

// A plain name that may be the numerator or denominator of an isotope ratio.
bool ratio_name(std::string_view s) {
  return !s.empty() && s.find('.') == std::string_view::npos && s.find('(') == std::string_view::npos &&
         !computed(s) && !is_keyword(s) && s != "device" && s != "param" && s != "gauge";
}

class Parser {
 public:
  Parser(std::string_view text, std::vector<Token> toks) : text_(text), toks_(std::move(toks)) {}

  Result<ExprPtr> parse() {
    auto e = or_expr();
    if (!e) return e;
    if (peek().t != Tok::End) return error("unexpected trailing input at " + std::to_string(peek().pos));
    return e;
  }

 private:
  std::string_view text_;
  std::vector<Token> toks_;
  size_t p_ = 0;
  bool series_ok_ = false;

  const Token& peek(size_t ahead = 0) const { return toks_[std::min(p_ + ahead, toks_.size() - 1)]; }
  const Token& next() { return toks_[p_ < toks_.size() - 1 ? p_++ : p_]; }
  bool kw(std::string_view w) const { return peek().t == Tok::Ident && peek().text == w; }

  Unexpected<Error> error(const std::string& m) const {
    return fail(ErrorKind::Config, "conditional '" + std::string(text_) + "': " + m);
  }

  Result<ExprPtr> or_expr() {
    auto l = and_expr();
    if (!l) return l;
    while (kw("or")) {
      next();
      auto r = and_expr();
      if (!r) return r;
      l = binary(Expr::Kind::Or, std::move(*l), std::move(*r));
    }
    return l;
  }

  Result<ExprPtr> and_expr() {
    auto l = not_expr();
    if (!l) return l;
    while (kw("and")) {
      next();
      auto r = not_expr();
      if (!r) return r;
      l = binary(Expr::Kind::And, std::move(*l), std::move(*r));
    }
    return l;
  }

  Result<ExprPtr> not_expr() {
    if (kw("not")) {
      next();
      auto x = not_expr();
      if (!x) return x;
      auto n = make(Expr::Kind::Not);
      n->children.push_back(std::move(*x));
      return n;
    }
    return cmp();
  }

  Result<ExprPtr> cmp() {
    auto l = sum();
    if (!l) return l;
    if (peek().t != Tok::Cmp) return l;
    CmpOp op = next().op;
    auto r = sum();
    if (!r) return r;
    auto n = binary(Expr::Kind::Cmp, std::move(*l), std::move(*r));
    n->op = op;
    return n;
  }

  Result<ExprPtr> sum() {
    auto l = term();
    if (!l) return l;
    while (peek().t == Tok::Plus || peek().t == Tok::Minus) {
      const auto k = next().t == Tok::Plus ? Expr::Kind::Add : Expr::Kind::Sub;
      auto r = term();
      if (!r) return r;
      l = binary(k, std::move(*l), std::move(*r));
    }
    return l;
  }

  Result<ExprPtr> term() {
    auto l = unary();
    if (!l) return l;
    while (peek().t == Tok::Star || peek().t == Tok::Slash) {
      const auto k = next().t == Tok::Star ? Expr::Kind::Mul : Expr::Kind::Div;
      auto r = unary();
      if (!r) return r;
      l = binary(k, std::move(*l), std::move(*r));
    }
    return l;
  }

  Result<ExprPtr> unary() {
    if (peek().t == Tok::Minus) {
      next();
      auto x = unary();
      if (!x) return x;
      if ((*x)->kind == Expr::Kind::Number) {  // fold literals: "-5" stays a number
        (*x)->number = -(*x)->number;
        return x;
      }
      auto n = make(Expr::Kind::Neg);
      n->children.push_back(std::move(*x));
      return n;
    }
    return atom();
  }

  Result<ExprPtr> series_arg() {
    series_ok_ = true;
    auto v = sum();
    series_ok_ = false;
    return v;
  }

  Result<ExprPtr> metric_node(MetricRef m) {
    if (is_series_only(m) && !series_ok_)
      return error("series '" + to_string(m) + "' used as a scalar without a reducer (use e.g. average(" +
                   to_string(m) + "))");
    auto n = make(Expr::Kind::Metric);
    n->metric = std::move(m);
    return n;
  }

  Result<ExprPtr> atom() {
    const Token t = next();
    switch (t.t) {
      case Tok::Num: {
        auto n = make(Expr::Kind::Number);
        n->number = t.num;
        return n;
      }
      case Tok::Var: {
        auto n = make(Expr::Kind::Var);
        n->var = t.text;
        return n;
      }
      case Tok::LParen: {
        bool saved = std::exchange(series_ok_, false);
        auto e = or_expr();
        series_ok_ = saved;
        if (!e) return e;
        if (next().t != Tok::RParen) return error("expected ')'");
        return e;
      }
      case Tok::Ident: return ident(t);
      default: return error("expected a value at " + std::to_string(t.pos));
    }
  }

  Result<ExprPtr> ident(const Token& t) {
    const std::string& s = t.text;
    if (is_keyword(s)) return error("unexpected '" + s + "'");
    if (peek().t == Tok::LParen && s.find('.') == std::string::npos) return call(s);
    // ISO/ISO with two bare names is a ratio metric.
    if (peek().t == Tok::Slash && peek(1).t == Tok::Ident && peek(2).t != Tok::LParen && ratio_name(s) &&
        ratio_name(peek(1).text)) {
      next();
      MetricRef m;
      m.kind = MetricRef::Kind::Ratio;
      m.a = s;
      m.b = next().text;
      return metric_node(std::move(m));
    }
    MetricRef m;
    if (computed(s)) {
      m.kind = MetricRef::Kind::Computed;
      m.a = s;
      return metric_node(std::move(m));
    }
    // Split at dots outside a "(...)" qualifier.
    std::vector<std::string> parts;
    size_t b = 0;
    int depth = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
      if (i < s.size() && s[i] == '(') ++depth;
      if (i < s.size() && s[i] == ')') --depth;
      if (i == s.size() || (s[i] == '.' && depth == 0)) {
        parts.push_back(s.substr(b, i - b));
        b = i + 1;
      }
    }
    if (parts[0] == "device" || parts[0] == "param") {
      if (parts.size() < 2) return error("'" + parts[0] + ".' needs a NAME");
      m.kind = parts[0] == "device" ? MetricRef::Kind::Device : MetricRef::Kind::Param;
      m.a = s.substr(parts[0].size() + 1);
    } else if (parts[0] == "gauge") {
      if (parts.size() < 3 || parts.back() != "pressure") return error("gauge metric must be gauge.NAME.pressure");
      m.kind = MetricRef::Kind::Gauge;
      m.a = s.substr(6, s.size() - 6 - 9);
      m.field = "pressure";
    } else if (parts.size() == 1) {
      if (s.find('(') != std::string::npos) return error("'" + s + "' needs a detector field (e.g. .deflection)");
      m.kind = MetricRef::Kind::Isotope;
      m.a = s;
    } else if (parts.size() == 2) {
      if (auto f = iso_field(parts[1]); f && parts[0].find('(') == std::string::npos) {
        m.kind = MetricRef::Kind::IsotopeField;
        m.field = std::string(*f);
      } else if (det_field(parts[1])) {
        m.kind = MetricRef::Kind::DetectorField;
        m.field = parts[1];
      } else {
        return error("unknown metric field '" + parts[1] + "' in '" + s + "'");
      }
      m.a = parts[0];
    } else {
      return error("unknown metric '" + s + "'");
    }
    return metric_node(std::move(m));
  }

  Result<ExprPtr> call(const std::string& name) {
    auto f = find_func(name);
    if (!f) return error("unknown function '" + name + "'");
    next();  // (
    auto n = make(Expr::Kind::Call);
    n->func = *f;
    bool first = true;
    while (peek().t != Tok::RParen) {
      if (!first) {
        if (next().t != Tok::Comma) return error("expected ',' in " + name + "()");
      }
      if (peek().t == Tok::Ident && peek().text == "window" && peek(1).t == Tok::Assign) {
        if (!is_series_func(*f)) return error("window= is only valid for series functions, not " + name + "()");
        next();
        next();
        if (peek().t != Tok::Num || peek().num < 1 || peek().num != static_cast<int>(peek().num))
          return error("window must be a positive integer");
        n->window = static_cast<int>(next().num);
      } else {
        auto a = (is_series_func(*f) && first) ? series_arg() : or_expr();
        if (!a) return a;
        n->children.push_back(std::move(*a));
      }
      first = false;
      if (peek().t == Tok::End) return error("expected ')'");
    }
    next();  // )
    const size_t argc = n->children.size();
    auto need = [&](size_t k) -> Result<ExprPtr> {
      if (argc != k) return error(name + "() takes " + std::to_string(k) + " argument(s)");
      return ExprPtr{};
    };
    if (*f == Func::Elapsed || *f == Func::Between || *f == Func::Abs) {
      if (auto r = need(*f == Func::Elapsed ? 0 : *f == Func::Abs ? 1 : 3); !r) return r;
    } else {
      if (auto r = need(1); !r) return r;
      if (n->children[0]->kind != Expr::Kind::Metric) return error(name + "() needs a metric series argument");
    }
    return n;
  }
};

std::string number_text(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.15g", v);
  if (std::strtod(buf, nullptr) != v) std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

int precedence(const Expr& e) {
  using K = Expr::Kind;
  switch (e.kind) {
    case K::Or: return 1;
    case K::And: return 2;
    case K::Not: return 3;
    case K::Cmp: return 4;
    case K::Add:
    case K::Sub: return 5;
    case K::Mul:
    case K::Div: return 6;
    case K::Neg: return 7;
    case K::Number: return e.number < 0 ? 7 : 8;
    default: return 8;
  }
}

std::string child_text(const Expr& parent, const Expr& child, bool right = false) {
  using K = Expr::Kind;
  const int pp = precedence(parent), cp = precedence(child);
  // Lower binds looser; at the same level comparisons never chain and a right
  // child keeps its explicit grouping (operators parse left-associative).
  const bool unary = parent.kind == K::Not || parent.kind == K::Neg;
  const bool parens = cp < pp || (cp == pp && !unary && (parent.kind == K::Cmp || right));
  auto s = to_string(child);
  return parens ? "(" + s + ")" : s;
}

template <class F>
void walk(const Expr& e, F&& f) {
  f(e);
  for (const auto& c : e.children) walk(*c, f);
}

// Replaces every node for which `pick` returns a replacement.
ExprPtr rewrite(const Expr& e, const std::function<ExprPtr(const Expr&)>& pick) {
  if (auto r = pick(e)) return r;
  auto n = std::make_unique<Expr>();
  n->kind = e.kind;
  n->number = e.number;
  n->metric = e.metric;
  n->var = e.var;
  n->func = e.func;
  n->window = e.window;
  n->op = e.op;
  for (const auto& c : e.children) n->children.push_back(rewrite(*c, pick));
  return n;
}

}  // namespace

std::string to_string(const MetricRef& m) {
  using K = MetricRef::Kind;
  switch (m.kind) {
    case K::Isotope: return m.a;
    case K::Ratio: return m.a + "/" + m.b;
    case K::IsotopeField:
    case K::DetectorField: return m.a + "." + m.field;
    case K::Computed: return m.a;
    case K::Device: return "device." + m.a;
    case K::Gauge: return "gauge." + m.a + ".pressure";
    case K::Param: return "param." + m.a;
  }
  return {};
}

bool is_series_only(const MetricRef& m) noexcept {
  return m.kind == MetricRef::Kind::IsotopeField && m.field == "bs";
}

std::span<const std::string_view> computed_metric_names() noexcept { return kComputed; }

std::string_view to_string(Func f) noexcept {
  for (const auto& e : kFuncs)
    if (e.func == f) return e.name;
  return "";
}

std::string_view to_string(CmpOp op) noexcept {
  switch (op) {
    case CmpOp::Lt: return "<";
    case CmpOp::Le: return "<=";
    case CmpOp::Gt: return ">";
    case CmpOp::Ge: return ">=";
    case CmpOp::Eq: return "==";
    case CmpOp::Ne: return "!=";
  }
  return "";
}

std::string to_string(const Expr& e) {
  using K = Expr::Kind;
  auto bin = [&](std::string_view op) {
    return child_text(e, *e.children[0]) + " " + std::string(op) + " " + child_text(e, *e.children[1], true);
  };
  switch (e.kind) {
    case K::Number: return number_text(e.number);
    case K::Metric: return to_string(e.metric);
    case K::Var: return "$" + e.var;
    case K::Call: {
      std::string s(to_string(e.func));
      s += "(";
      for (size_t i = 0; i < e.children.size(); ++i) {
        if (i) s += ", ";
        s += to_string(*e.children[i]);
      }
      if (e.window) s += std::string(e.children.empty() ? "" : ", ") + "window=" + std::to_string(*e.window);
      return s + ")";
    }
    case K::Cmp: return bin(to_string(e.op));
    case K::And: return bin("and");
    case K::Or: return bin("or");
    case K::Not: return "not " + child_text(e, *e.children[0]);
    case K::Neg: return "-" + child_text(e, *e.children[0]);
    case K::Add: return bin("+");
    case K::Sub: return bin("-");
    case K::Mul: return bin("*");
    case K::Div: return bin("/");
  }
  return {};
}

Result<ExprPtr> parse_expression(std::string_view text) {
  auto toks = tokenize(text);
  if (!toks) return fail(toks.error());
  return Parser(text, std::move(*toks)).parse();
}

ExprPtr clone(const Expr& e) {
  return rewrite(e, [](const Expr&) { return ExprPtr{}; });
}

std::vector<MetricRef> metrics_of(const Expr& e) {
  std::vector<MetricRef> out;
  walk(e, [&](const Expr& n) {
    if (n.kind == Expr::Kind::Metric && std::find(out.begin(), out.end(), n.metric) == out.end())
      out.push_back(n.metric);
  });
  return out;
}

std::vector<std::string> variables_of(const Expr& e) {
  std::vector<std::string> out;
  walk(e, [&](const Expr& n) {
    if (n.kind == Expr::Kind::Var && std::find(out.begin(), out.end(), n.var) == out.end()) out.push_back(n.var);
  });
  return out;
}

ExprPtr apply_window(const Expr& e, int window) {
  return rewrite(e, [&](const Expr& n) -> ExprPtr {
    if (n.kind == Expr::Kind::Call && is_series_func(n.func)) {
      auto c = clone(n);  // its argument is a series: never wrapped
      if (!c->window) c->window = window;
      return c;
    }
    if (n.kind == Expr::Kind::Metric && n.metric.kind == MetricRef::Kind::Isotope) {
      auto call = make(Expr::Kind::Call);
      call->func = Func::Average;
      call->window = window;
      call->children.push_back(clone(n));
      return call;
    }
    return ExprPtr{};
  });
}

Result<ExprPtr> apply_mapper(const Expr& e, std::string_view mapper) {
  auto m = parse_expression(mapper);
  if (!m) return fail(ErrorKind::Config, "mapper: " + m.error().what);
  bool has_x = false, valid = true;
  walk(**m, [&](const Expr& n) {
    using K = Expr::Kind;
    if (n.kind == K::Metric) {
      if (n.metric.kind == MetricRef::Kind::Isotope && n.metric.a == "x") has_x = true;
      else valid = false;
    }
    if (n.kind == K::Cmp || n.kind == K::And || n.kind == K::Or || n.kind == K::Not) valid = false;
  });
  if (!valid || !has_x)
    return fail(ErrorKind::Config, "mapper '" + std::string(mapper) + "' must be an arithmetic expression in x");
  const Expr& map = **m;
  return rewrite(e, [&](const Expr& n) -> ExprPtr {
    const bool value = n.kind == Expr::Kind::Metric || (n.kind == Expr::Kind::Call && is_series_func(n.func));
    if (!value) return ExprPtr{};
    return rewrite(map, [&](const Expr& x) -> ExprPtr {
      if (x.kind == Expr::Kind::Metric && x.metric.kind == MetricRef::Kind::Isotope && x.metric.a == "x") return clone(n);
      return ExprPtr{};
    });
  });
}

}  // namespace pychron::experiment
