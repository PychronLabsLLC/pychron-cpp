// UFloat: a value with first-order uncertainty propagation (spec 4).
//
// A UFloat is a nominal value plus a list of terms, one per independent
// variable it depends on, sorted by variable id. Each term carries the
// variable's sigma and tag inline, so there is no registry. Arithmetic merges
// term lists in one linear pass and evaluates the same analytic partials as
// the Python `uncertainties` package, which legacy pychron uses (spec 4.3,
// 4.7). Correlation arises only through shared variables (spec 4.1).
//
// Header-only, Qt-free, I/O-free, never throws (spec 4.3).
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pychron::reduction {

using VariableId = std::uint64_t;  // process-unique, never 0, never reused
using TagId = std::uint32_t;       // 0 = untagged

namespace detail {

// Process-wide variable id source (spec 4.4). Starts at 1 so 0 means "none".
inline std::atomic<std::uint64_t>& variable_id_counter() noexcept {
  static std::atomic<std::uint64_t> counter{1};
  return counter;
}

// Append-only tag table (spec 4.5). Names live in a deque so string_views into
// it stay valid as it grows; the map keys view the same storage.
struct TagTable {
  std::mutex mutex;
  std::deque<std::string> names;  // names[i] is TagId i + 1
  std::unordered_map<std::string_view, TagId> ids;
};

inline TagTable& tag_table() {
  static TagTable table;
  return table;
}

}  // namespace detail

// Interned, process-wide, append-only, thread-safe. Names are never freed.
// The empty name is the untagged tag 0.
inline TagId intern_tag(std::string_view name) {
  if (name.empty()) return 0;
  detail::TagTable& table = detail::tag_table();
  const std::lock_guard<std::mutex> lock(table.mutex);
  if (auto it = table.ids.find(name); it != table.ids.end()) return it->second;
  table.names.emplace_back(name);
  const auto id = static_cast<TagId>(table.names.size());
  table.ids.emplace(std::string_view(table.names.back()), id);
  return id;
}

// "" for 0 or an unknown id.
inline std::string_view tag_name(TagId tag) {
  if (tag == 0) return {};
  detail::TagTable& table = detail::tag_table();
  const std::lock_guard<std::mutex> lock(table.mutex);
  if (tag > table.names.size()) return {};
  return table.names[tag - 1];
}

class UFloat {
 public:
  struct Term {
    VariableId id;
    double sigma;  // the variable's standard deviation (immutable)
    double deriv;  // d(this)/d(variable)
    TagId tag;
  };

  UFloat() noexcept = default;  // exact 0
  // Implicit: an exact constant with no terms.
  UFloat(double exact) noexcept : nominal_(exact) {}  // NOLINT(google-explicit-constructor)

  // New independent variable. sigma must be finite and >= 0 (asserted; inputs
  // are validated at the reduction API boundary). sigma == 0 returns an exact
  // constant with no term (spec 4.4).
  static UFloat variable(double value, double sigma, TagId tag = 0) {
    assert(std::isfinite(sigma) && sigma >= 0.0 && "UFloat::variable: sigma must be finite >= 0");
    UFloat out(value);
    if (sigma == 0.0) return out;
    const VariableId id = detail::variable_id_counter().fetch_add(1, std::memory_order_relaxed);
    out.terms_.push_back(Term{id, sigma, 1.0, tag});
    return out;
  }
  static UFloat variable(double value, double sigma, std::string_view tag) {
    return variable(value, sigma, intern_tag(tag));
  }

  double nominal() const noexcept { return nominal_; }
  // Plain unscaled sum of squares, deliberately: parity with `uncertainties`
  // (spec 4.7); do not switch to hypot-style scaling.
  double variance() const noexcept {
    double v = 0.0;
    for (const Term& t : terms_) {
      const double c = t.deriv * t.sigma;
      v += c * c;
    }
    return v;
  }
  double std_dev() const noexcept { return std::sqrt(variance()); }
  bool is_exact() const noexcept { return terms_.empty(); }
  std::span<const Term> terms() const noexcept { return terms_; }  // ascending id

  // 0 if this does not depend on `id`.
  double derivative(VariableId id) const noexcept {
    const auto it = std::lower_bound(terms_.begin(), terms_.end(), id,
                                     [](const Term& t, VariableId v) { return t.id < v; });
    return it != terms_.end() && it->id == id ? it->deriv : 0.0;
  }

  // Single-term UFloats (fresh variables) expose their id; 0 otherwise.
  VariableId variable_id() const noexcept { return terms_.size() == 1 ? terms_[0].id : 0; }

  UFloat& operator+=(const UFloat& y) { return *this = *this + y; }
  UFloat& operator-=(const UFloat& y) { return *this = *this - y; }
  UFloat& operator*=(const UFloat& y) { return *this = *this * y; }
  UFloat& operator/=(const UFloat& y) { return *this = *this / y; }
  UFloat& operator+=(double y) { return *this = *this + y; }
  UFloat& operator-=(double y) { return *this = *this - y; }
  UFloat& operator*=(double y) { return *this = *this * y; }
  UFloat& operator/=(double y) { return *this = *this / y; }

  friend UFloat operator-(const UFloat& x) { return combine(-x.nominal_, x, -1.0, {}, 0.0); }

  // Derivative rules per spec 4.3, with the partials `uncertainties` uses.
  friend UFloat operator+(const UFloat& x, const UFloat& y) {
    return combine(x.nominal_ + y.nominal_, x, 1.0, y, 1.0);
  }
  friend UFloat operator-(const UFloat& x, const UFloat& y) {
    return combine(x.nominal_ - y.nominal_, x, 1.0, y, -1.0);
  }
  friend UFloat operator*(const UFloat& x, const UFloat& y) {
    return combine(x.nominal_ * y.nominal_, x, y.nominal_, y, x.nominal_);
  }
  friend UFloat operator/(const UFloat& x, const UFloat& y) {
    const double y0 = y.nominal_;
    return combine(x.nominal_ / y0, x, 1.0 / y0, y, -x.nominal_ / (y0 * y0));
  }

  friend UFloat operator+(const UFloat& x, double c) { return combine(x.nominal_ + c, x, 1.0, {}, 0.0); }
  friend UFloat operator+(double c, const UFloat& y) { return combine(c + y.nominal_, y, 1.0, {}, 0.0); }
  friend UFloat operator-(const UFloat& x, double c) { return combine(x.nominal_ - c, x, 1.0, {}, 0.0); }
  friend UFloat operator-(double c, const UFloat& y) { return combine(c - y.nominal_, y, -1.0, {}, 0.0); }
  friend UFloat operator*(const UFloat& x, double c) { return combine(x.nominal_ * c, x, c, {}, 0.0); }
  friend UFloat operator*(double c, const UFloat& y) { return combine(c * y.nominal_, y, c, {}, 0.0); }
  friend UFloat operator/(const UFloat& x, double c) { return combine(x.nominal_ / c, x, 1.0 / c, {}, 0.0); }
  friend UFloat operator/(double c, const UFloat& y) {
    const double y0 = y.nominal_;
    return combine(c / y0, y, -c / (y0 * y0), {}, 0.0);
  }

 private:
  friend UFloat exp(const UFloat& x);
  friend UFloat log(const UFloat& x);
  friend UFloat log10(const UFloat& x);
  friend UFloat sqrt(const UFloat& x);
  friend UFloat abs(const UFloat& x);
  friend UFloat pow(const UFloat& x, double c);
  friend UFloat pow(double c, const UFloat& y);
  friend UFloat pow(const UFloat& x, const UFloat& y);

  // The one merge routine: a UFloat with nominal `nominal` and derivatives
  // ca * da + cb * db, merged by id in one pass. A derivative present on only
  // one side is scaled by that side's coefficient alone (never multiplied
  // against an absent 0, so an infinite coefficient cannot inject NaN). A
  // merged derivative that is exactly 0.0 is dropped, so x - x is exact zero.
  static UFloat combine(double nominal, const UFloat& a, double ca, const UFloat& b, double cb) {
    UFloat out(nominal);
    const std::vector<Term>& ta = a.terms_;
    const std::vector<Term>& tb = b.terms_;
    out.terms_.reserve(ta.size() + tb.size());
    auto push = [&out](const Term& t, double deriv) {
      if (deriv != 0.0) out.terms_.push_back(Term{t.id, t.sigma, deriv, t.tag});
    };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < ta.size() && j < tb.size()) {
      if (ta[i].id < tb[j].id) {
        push(ta[i], ca * ta[i].deriv);
        ++i;
      } else if (tb[j].id < ta[i].id) {
        push(tb[j], cb * tb[j].deriv);
        ++j;
      } else {
        push(ta[i], ca * ta[i].deriv + cb * tb[j].deriv);
        ++i;
        ++j;
      }
    }
    for (; i < ta.size(); ++i) push(ta[i], ca * ta[i].deriv);
    for (; j < tb.size(); ++j) push(tb[j], cb * tb[j].deriv);
    return out;
  }

  double nominal_ = 0.0;
  std::vector<Term> terms_;  // ascending id, no zero derivatives
};

namespace detail {

// d(x^c)/dx at x0 (spec 4.3): c == 0 gives 0 (so pow(x, 0) is exact 1), and
// x0 == 0 with c > 1 gives 0. `uncertainties` 3.2.3 agrees except for a
// non-integer c > 1 at x0 == 0, where it reports NaN; spec 4.3 defines 0.
// Otherwise c x0^(c-1), IEEE (inf/NaN) at the remaining singular points.
inline double pow_deriv_base(double x0, double c) noexcept {
  if (c == 0.0) return 0.0;
  if (x0 == 0.0 && c > 1.0) return 0.0;
  return c * std::pow(x0, c - 1.0);
}

// d(b^y)/dy at (b, y0): ln(b) b^y0, and 0 when b == 0 and y0 > 0 (the
// `uncertainties` 3.2.3 special case; avoids ln(0) * 0 = NaN).
inline double pow_deriv_exponent(double b, double y0) noexcept {
  if (b == 0.0 && y0 > 0.0) return 0.0;
  return std::log(b) * std::pow(b, y0);
}

}  // namespace detail

// Functions (spec 4.3). Domain errors follow IEEE (NaN/inf); never throw.
inline UFloat exp(const UFloat& x) {
  const double e = std::exp(x.nominal_);
  return UFloat::combine(e, x, e, {}, 0.0);
}
inline UFloat log(const UFloat& x) {
  return UFloat::combine(std::log(x.nominal_), x, 1.0 / x.nominal_, {}, 0.0);
}
inline UFloat log10(const UFloat& x) {
  return UFloat::combine(std::log10(x.nominal_), x, 1.0 / (x.nominal_ * std::log(10.0)), {},
                         0.0);
}
inline UFloat sqrt(const UFloat& x) {
  const double r = std::sqrt(x.nominal_);
  return UFloat::combine(r, x, 0.5 / r, {}, 0.0);
}
// Derivative 1 for x0 >= 0, else -1 (as `uncertainties`).
inline UFloat abs(const UFloat& x) {
  return UFloat::combine(std::fabs(x.nominal_), x, x.nominal_ >= 0.0 ? 1.0 : -1.0, {}, 0.0);
}
inline UFloat pow(const UFloat& x, double c) {
  return UFloat::combine(std::pow(x.nominal_, c), x, detail::pow_deriv_base(x.nominal_, c), {},
                         0.0);
}
inline UFloat pow(double c, const UFloat& y) {
  return UFloat::combine(std::pow(c, y.nominal_), y, detail::pow_deriv_exponent(c, y.nominal_),
                         {}, 0.0);
}
inline UFloat pow(const UFloat& x, const UFloat& y) {
  const double x0 = x.nominal_;
  const double y0 = y.nominal_;
  return UFloat::combine(std::pow(x0, y0), x, detail::pow_deriv_base(x0, y0), y,
                         detail::pow_deriv_exponent(x0, y0));
}

// sum over shared variables of d_a d_b sigma^2 (spec 4.1), one merge pass.
inline double covariance(const UFloat& a, const UFloat& b) noexcept {
  const auto ta = a.terms();
  const auto tb = b.terms();
  double cov = 0.0;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < ta.size() && j < tb.size()) {
    if (ta[i].id < tb[j].id) {
      ++i;
    } else if (tb[j].id < ta[i].id) {
      ++j;
    } else {
      cov += ta[i].deriv * tb[j].deriv * ta[i].sigma * ta[i].sigma;
      ++i;
      ++j;
    }
  }
  return cov;
}

// 0 if either standard deviation is 0.
inline double correlation(const UFloat& a, const UFloat& b) noexcept {
  const double sa = a.std_dev();
  const double sb = b.std_dev();
  if (sa == 0.0 || sb == 0.0) return 0.0;
  return covariance(a, b) / (sa * sb);
}

// Row-major n*n, symmetric (each pair computed once).
inline std::vector<double> covariance_matrix(std::span<const UFloat> xs) {
  const std::size_t n = xs.size();
  std::vector<double> m(n * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    m[i * n + i] = xs[i].variance();
    for (std::size_t j = i + 1; j < n; ++j) {
      const double c = covariance(xs[i], xs[j]);
      m[i * n + j] = c;
      m[j * n + i] = c;
    }
  }
  return m;
}

// Error budget helpers. The std of x with the listed variables treated as exact.
inline double std_dev_excluding(const UFloat& x, std::span<const VariableId> ids) {
  double v = 0.0;
  for (const UFloat::Term& t : x.terms()) {
    if (std::find(ids.begin(), ids.end(), t.id) != ids.end()) continue;
    const double c = t.deriv * t.sigma;
    v += c * c;
  }
  return std::sqrt(v);
}

inline double std_dev_excluding_tags(const UFloat& x, std::span<const TagId> tags) {
  double v = 0.0;
  for (const UFloat::Term& t : x.terms()) {
    if (std::find(tags.begin(), tags.end(), t.tag) != tags.end()) continue;
    const double c = t.deriv * t.sigma;
    v += c * c;
  }
  return std::sqrt(v);
}

// Per tag (0 = untagged included): sqrt(sum (d sigma)^2), ascending TagId.
inline std::vector<std::pair<TagId, double>> error_components(const UFloat& x) {
  std::vector<std::pair<TagId, double>> out;  // (tag, variance) until the end
  for (const UFloat::Term& t : x.terms()) {
    const double c = t.deriv * t.sigma;
    auto it = std::lower_bound(out.begin(), out.end(), t.tag,
                               [](const auto& p, TagId tag) { return p.first < tag; });
    if (it != out.end() && it->first == t.tag) {
      it->second += c * c;
    } else {
      out.insert(it, {t.tag, c * c});
    }
  }
  for (auto& p : out) p.second = std::sqrt(p.second);
  return out;
}

// E20: 100 * (variance from variables tagged `tag`) / variance; 0 when the
// variance is 0.
inline double variance_percent(const UFloat& x, TagId tag) noexcept {
  const double total = x.variance();
  if (total == 0.0) return 0.0;
  double v = 0.0;
  for (const UFloat::Term& t : x.terms()) {
    if (t.tag != tag) continue;
    const double c = t.deriv * t.sigma;
    v += c * c;
  }
  return 100.0 * v / total;
}

}  // namespace pychron::reduction
