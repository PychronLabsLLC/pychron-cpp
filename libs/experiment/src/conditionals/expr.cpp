#include "pychron/experiment/conditionals/expr.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pychron::experiment {
namespace {

constexpr std::string_view kIsoFields[] = {"cur", "bs", "bs_corrected", "ic_corrected"};
constexpr std::string_view kDetFields[] = {"deflection", "inactive", "intensity"};

bool one_of(std::string_view s, const std::string_view* b, const std::string_view* e) {
  for (; b != e; ++b)
    if (*b == s) return true;
  return false;
}

struct FuncName {
  std::string_view name;
  Func func;
};
constexpr FuncName kFuncs[] = {{"min", Func::Min},         {"max", Func::Max},         {"average", Func::Average},
                               {"slope", Func::Slope},     {"std", Func::Std},         {"rsd", Func::Rsd},
                               {"between", Func::Between}, {"count", Func::Count},     {"elapsed", Func::Elapsed}};

std::optional<Func> find_func(std::string_view s) {
  for (const auto& f : kFuncs)
    if (f.name == s) return f.func;
  return std::nullopt;
}

bool is_series_func(Func f) {
  return f == Func::Min || f == Func::Max || f == Func::Average || f == Func::Slope || f == Func::Std ||
         f == Func::Rsd || f == Func::Count;
}

enum class Tok { End, Num, Ident, Var, LParen, RParen, Comma, Slash, Assign, Cmp, Minus };
struct Token {
  Tok t = Tok::End;
  std::string text;
  double num = 0;
  CmpOp op = CmpOp::Lt;
  size_t pos = 0;
};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

Result<std::vector<Token>> tokenize(std::string_view s) {
  std::vector<Token> out;
  size_t i = 0;
  auto err = [&](const std::string& m) { return fail(ErrorKind::Config, "conditional '" + std::string(s) + "': " + m); };
  while (i < s.size()) {
    char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    Token t;
    t.pos = i;
    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
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
      else if (c == '=') { t.t = Tok::Assign; i += 1; out.push_back(t); continue; }
      else return err("unexpected '!' at " + std::to_string(i));
      i += eq ? 2 : 1;
    } else {
      switch (c) {
        case '(': t.t = Tok::LParen; break;
        case ')': t.t = Tok::RParen; break;
        case ',': t.t = Tok::Comma; break;
        case '/': t.t = Tok::Slash; break;
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

  const Token& peek() const { return toks_[p_]; }
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
      auto n = make(Expr::Kind::Or);
      n->children.push_back(std::move(*l));
      n->children.push_back(std::move(*r));
      l = std::move(n);
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
      auto n = make(Expr::Kind::And);
      n->children.push_back(std::move(*l));
      n->children.push_back(std::move(*r));
      l = std::move(n);
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
    auto l = value();
    if (!l) return l;
    if (peek().t != Tok::Cmp) return l;
    CmpOp op = next().op;
    auto r = value();
    if (!r) return r;
    auto n = make(Expr::Kind::Cmp);
    n->op = op;
    n->children.push_back(std::move(*l));
    n->children.push_back(std::move(*r));
    return n;
  }

  Result<ExprPtr> series_arg() {
    series_ok_ = true;
    auto v = value();
    series_ok_ = false;
    return v;
  }

  Result<ExprPtr> metric_node(MetricRef m) {
    if (is_series_only(m) && !series_ok_)
      return error("series '" + to_string(m) + "' used as a scalar without a reducer (use e.g. average(" + to_string(m) + "))");
    auto n = make(Expr::Kind::Metric);
    n->metric = std::move(m);
    return n;
  }

  Result<ExprPtr> value() {
    const Token t = next();
    switch (t.t) {
      case Tok::Num: {
        auto n = make(Expr::Kind::Number);
        n->number = t.num;
        return n;
      }
      case Tok::Minus: {
        if (peek().t != Tok::Num) return error("expected a number after '-' at " + std::to_string(t.pos));
        auto n = make(Expr::Kind::Number);
        n->number = -next().num;
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
      case Tok::Ident:
        return ident(t);
      default:
        return error("expected a value at " + std::to_string(t.pos));
    }
  }

  Result<ExprPtr> ident(const Token& t) {
    const std::string& s = t.text;
    if (peek().t == Tok::LParen && s.find('.') == std::string::npos) return call(s);
    if (peek().t == Tok::Slash) {
      next();
      if (peek().t != Tok::Ident || peek().text.find('.') != std::string::npos || s.find('.') != std::string::npos)
        return error("ratio must be ISO/ISO");
      MetricRef m;
      m.kind = MetricRef::Kind::Ratio;
      m.a = s;
      m.b = next().text;
      return metric_node(std::move(m));
    }
    MetricRef m;
    if (s == "age") m.kind = MetricRef::Kind::Age;
    else if (s == "kca") m.kind = MetricRef::Kind::Kca;
    else if (s == "radiogenic_yield") m.kind = MetricRef::Kind::RadiogenicYield;
    else if (s == "and" || s == "or" || s == "not") return error("unexpected '" + s + "'");
    else {
      std::vector<std::string> parts;
      size_t b = 0;
      for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '.') { parts.push_back(s.substr(b, i - b)); b = i + 1; }
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
        m.kind = MetricRef::Kind::Isotope;
        m.a = s;
      } else if (parts.size() == 2) {
        if (one_of(parts[1], std::begin(kIsoFields), std::end(kIsoFields))) m.kind = MetricRef::Kind::IsotopeField;
        else if (one_of(parts[1], std::begin(kDetFields), std::end(kDetFields))) m.kind = MetricRef::Kind::DetectorField;
        else return error("unknown metric field '" + parts[1] + "' in '" + s + "'");
        m.a = parts[0];
        m.field = parts[1];
      } else {
        return error("unknown metric '" + s + "'");
      }
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
      if (peek().t == Tok::Ident && peek().text == "window" && toks_[p_ + 1].t == Tok::Assign) {
        if (!is_series_func(*f)) return error("window= is only valid for series functions, not " + name + "()");
        next(); next();
        bool neg = peek().t == Tok::Minus;
        if (neg) next();
        if (peek().t != Tok::Num || neg || peek().num < 1 || peek().num != static_cast<int>(peek().num))
          return error("window must be a positive integer");
        n->window = static_cast<int>(next().num);
      } else {
        auto a = (is_series_func(*f) && first) ? series_arg() : value();
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
    if (*f == Func::Elapsed || *f == Func::Between) {
      if (auto r = need(*f == Func::Elapsed ? 0 : 3); !r) return r;
    } else {
      if (auto r = need(1); !r) return r;
      if (n->children[0]->kind != Expr::Kind::Metric)
        return error(name + "() needs a metric series argument");
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

bool needs_parens(const Expr& parent, const Expr& child) {
  using K = Expr::Kind;
  switch (parent.kind) {
    case K::Cmp: return child.kind == K::And || child.kind == K::Or || child.kind == K::Not || child.kind == K::Cmp;
    case K::Not: return child.kind == K::And || child.kind == K::Or;
    case K::And: return child.kind == K::Or;
    default: return false;
  }
}

std::string child_text(const Expr& parent, const Expr& child) {
  auto s = to_string(child);
  return needs_parens(parent, child) ? "(" + s + ")" : s;
}

}  // namespace

std::string to_string(const MetricRef& m) {
  using K = MetricRef::Kind;
  switch (m.kind) {
    case K::Isotope: return m.a;
    case K::Ratio: return m.a + "/" + m.b;
    case K::IsotopeField:
    case K::DetectorField: return m.a + "." + m.field;
    case K::Age: return "age";
    case K::Kca: return "kca";
    case K::RadiogenicYield: return "radiogenic_yield";
    case K::Device: return "device." + m.a;
    case K::Gauge: return "gauge." + m.a + ".pressure";
    case K::Param: return "param." + m.a;
  }
  return {};
}

bool is_series_only(const MetricRef& m) noexcept {
  return m.kind == MetricRef::Kind::IsotopeField && m.field == "bs";
}

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
    case K::Cmp:
      return child_text(e, *e.children[0]) + " " + std::string(to_string(e.op)) + " " + child_text(e, *e.children[1]);
    case K::And: return child_text(e, *e.children[0]) + " and " + child_text(e, *e.children[1]);
    case K::Or: return child_text(e, *e.children[0]) + " or " + child_text(e, *e.children[1]);
    case K::Not: return "not " + child_text(e, *e.children[0]);
  }
  return {};
}

Result<ExprPtr> parse_expression(std::string_view text) {
  auto toks = tokenize(text);
  if (!toks) return fail(toks.error());
  return Parser(text, std::move(*toks)).parse();
}

}  // namespace pychron::experiment
