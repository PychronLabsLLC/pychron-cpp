#include "pychron/experiment/conditionals/evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pychron::experiment {
namespace {

Unexpected<Error> eval_fail(const std::string& what) { return fail(ErrorKind::Config, what); }

Result<std::vector<double>> windowed_series(const Expr& call, const MetricContext& ctx) {
  const MetricRef& m = call.children[0]->metric;
  auto s = ctx.series(m);
  if (!s || s->empty()) return eval_fail("no series for '" + to_string(m) + "'");
  if (call.window && static_cast<size_t>(*call.window) < s->size())
    s->erase(s->begin(), s->end() - *call.window);
  return std::move(*s);
}

double mean(const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); }

double pstd(const std::vector<double>& v) {
  const double mu = mean(v);
  double ss = 0;
  for (double x : v) ss += (x - mu) * (x - mu);
  return std::sqrt(ss / static_cast<double>(v.size()));
}

// Least-squares slope of value against reading index.
double slope(const std::vector<double>& v) {
  const auto n = static_cast<double>(v.size());
  if (v.size() < 2) return 0;
  const double xm = (n - 1) / 2, ym = mean(v);
  double sxy = 0, sxx = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    const double dx = static_cast<double>(i) - xm;
    sxy += dx * (v[i] - ym);
    sxx += dx * dx;
  }
  return sxy / sxx;
}

Result<double> compare(CmpOp op, double l, double r) {
  switch (op) {
    case CmpOp::Lt: return l < r ? 1.0 : 0.0;
    case CmpOp::Le: return l <= r ? 1.0 : 0.0;
    case CmpOp::Gt: return l > r ? 1.0 : 0.0;
    case CmpOp::Ge: return l >= r ? 1.0 : 0.0;
    case CmpOp::Eq: return l == r ? 1.0 : 0.0;
    case CmpOp::Ne: return l != r ? 1.0 : 0.0;
  }
  return 0.0;
}

}  // namespace

std::optional<double> Variables::lookup(std::string_view name) const {
  const std::string key(name);
  if (auto it = script_options.find(key); it != script_options.end()) return it->second;
  if (auto it = params.find(key); it != params.end()) return it->second;
  return std::nullopt;
}

Result<double> evaluate(const Expr& e, const MetricContext& ctx, const Variables& vars) {
  using K = Expr::Kind;
  switch (e.kind) {
    case K::Number: return e.number;
    case K::Var: {
      if (auto v = vars.lookup(e.var)) return *v;
      return eval_fail("unresolved variable $" + e.var);
    }
    case K::Metric: {
      if (auto v = ctx.scalar(e.metric)) return *v;
      return eval_fail("metric '" + to_string(e.metric) + "' unavailable");
    }
    case K::Call: {
      if (e.func == Func::Elapsed) {
        if (auto v = ctx.elapsed()) return *v;
        return eval_fail("elapsed time unavailable");
      }
      if (e.func == Func::Abs) {
        auto v = evaluate(*e.children[0], ctx, vars);
        if (!v) return v;
        return std::fabs(*v);
      }
      if (e.func == Func::Between) {
        double v[3];
        for (int i = 0; i < 3; ++i) {
          auto r = evaluate(*e.children[static_cast<size_t>(i)], ctx, vars);
          if (!r) return r;
          v[i] = *r;
        }
        return (v[0] >= v[1] && v[0] <= v[2]) ? 1.0 : 0.0;
      }
      auto s = windowed_series(e, ctx);
      if (!s) return fail(s.error());
      switch (e.func) {
        case Func::Min: return *std::min_element(s->begin(), s->end());
        case Func::Max: return *std::max_element(s->begin(), s->end());
        case Func::Average: return mean(*s);
        case Func::Slope: return slope(*s);
        case Func::Std: return pstd(*s);
        case Func::Rsd: {
          const double mu = mean(*s);
          if (mu == 0) return eval_fail("rsd of zero-mean series");
          return 100.0 * pstd(*s) / std::fabs(mu);
        }
        case Func::Count: return static_cast<double>(s->size());
        default: break;
      }
      return eval_fail("bad function");
    }
    case K::Cmp: {
      auto l = evaluate(*e.children[0], ctx, vars);
      if (!l) return l;
      auto r = evaluate(*e.children[1], ctx, vars);
      if (!r) return r;
      return compare(e.op, *l, *r);
    }
    case K::And: {
      auto l = evaluate(*e.children[0], ctx, vars);
      if (!l) return l;
      if (*l == 0) return 0.0;
      auto r = evaluate(*e.children[1], ctx, vars);
      if (!r) return r;
      return *r != 0 ? 1.0 : 0.0;
    }
    case K::Or: {
      auto l = evaluate(*e.children[0], ctx, vars);
      if (!l) return l;
      if (*l != 0) return 1.0;
      auto r = evaluate(*e.children[1], ctx, vars);
      if (!r) return r;
      return *r != 0 ? 1.0 : 0.0;
    }
    case K::Not: {
      auto x = evaluate(*e.children[0], ctx, vars);
      if (!x) return x;
      return *x == 0 ? 1.0 : 0.0;
    }
    case K::Neg: {
      auto x = evaluate(*e.children[0], ctx, vars);
      if (!x) return x;
      return -*x;
    }
    case K::Add:
    case K::Sub:
    case K::Mul:
    case K::Div: {
      auto l = evaluate(*e.children[0], ctx, vars);
      if (!l) return l;
      auto r = evaluate(*e.children[1], ctx, vars);
      if (!r) return r;
      if (e.kind == K::Add) return *l + *r;
      if (e.kind == K::Sub) return *l - *r;
      if (e.kind == K::Mul) return *l * *r;
      if (*r == 0) return eval_fail("division by zero");
      return *l / *r;
    }
  }
  return eval_fail("bad expression");
}

Result<CheckResult> evaluate_check(const Expr& e, const MetricContext& ctx, const Variables& vars) {
  CheckResult out;
  auto r = evaluate(e, ctx, vars);
  if (!r) return fail(r.error());
  out.tripped = *r != 0;
  out.value = *r;
  if (e.kind == Expr::Kind::Cmp) {
    auto l = evaluate(*e.children[0], ctx, vars);
    if (!l) return fail(l.error());
    out.value = *l;
  }
  return out;
}

std::optional<std::vector<double>> MapContext::series(const MetricRef& m) const {
  auto it = series_data.find(to_string(m));
  if (it == series_data.end()) return std::nullopt;
  return it->second;
}

std::optional<double> MapContext::scalar(const MetricRef& m) const {
  auto it = series_data.find(to_string(m));
  if (it == series_data.end() || it->second.empty()) return std::nullopt;
  return it->second.back();
}

}  // namespace pychron::experiment
