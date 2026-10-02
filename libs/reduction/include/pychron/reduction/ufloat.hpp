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

}  // namespace pychron::reduction
