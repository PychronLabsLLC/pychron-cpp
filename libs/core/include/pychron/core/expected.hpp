#pragma once

// pychron::Expected / pychron::Unexpected.
//
// The spec calls for std::expected, which is a C++23 library feature. The
// project builds as C++20, where libc++ and MSVC do not ship it, so when
// <expected> is unavailable this header provides a minimal, API-compatible
// subset. Code must only use the members defined here so that it compiles
// against both implementations.

#if __has_include(<version>)
#include <version>
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>

namespace pychron {
template <class T, class E>
using Expected = std::expected<T, E>;
template <class E>
using Unexpected = std::unexpected<E>;
}  // namespace pychron

#else

#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <variant>

namespace pychron {

template <class E>
class Unexpected {
 public:
  constexpr explicit Unexpected(const E& e) : error_(e) {}
  constexpr explicit Unexpected(E&& e) : error_(std::move(e)) {}

  constexpr const E& error() const& noexcept { return error_; }
  constexpr E& error() & noexcept { return error_; }
  constexpr E&& error() && noexcept { return std::move(error_); }

 private:
  E error_;
};

template <class E>
Unexpected(E) -> Unexpected<E>;

namespace detail {
[[noreturn]] inline void bad_expected_access(const char* what) {
  std::fprintf(stderr, "pychron::Expected: %s\n", what);
  std::abort();
}
}  // namespace detail

template <class T, class E>
class Expected {
 public:
  using value_type = T;
  using error_type = E;

  constexpr Expected()
    requires std::is_default_constructible_v<T>
      : storage_(std::in_place_index<0>) {}

  template <class U = T>
    requires std::is_constructible_v<T, U&&> &&
             (!std::is_same_v<std::remove_cvref_t<U>, Expected>) &&
             (!std::is_same_v<std::remove_cvref_t<U>, Unexpected<E>>)
  constexpr Expected(U&& v) : storage_(std::in_place_index<0>, std::forward<U>(v)) {}  // NOLINT

  template <class G>
  constexpr Expected(const Unexpected<G>& u) : storage_(std::in_place_index<1>, u.error()) {}  // NOLINT
  template <class G>
  constexpr Expected(Unexpected<G>&& u)  // NOLINT
      : storage_(std::in_place_index<1>, std::move(u).error()) {}

  constexpr bool has_value() const noexcept { return storage_.index() == 0; }
  constexpr explicit operator bool() const noexcept { return has_value(); }

  constexpr T& value() & {
    check_value();
    return std::get<0>(storage_);
  }
  constexpr const T& value() const& {
    check_value();
    return std::get<0>(storage_);
  }
  constexpr T&& value() && {
    check_value();
    return std::get<0>(std::move(storage_));
  }

  constexpr E& error() & {
    check_error();
    return std::get<1>(storage_);
  }
  constexpr const E& error() const& {
    check_error();
    return std::get<1>(storage_);
  }
  constexpr E&& error() && {
    check_error();
    return std::get<1>(std::move(storage_));
  }

  constexpr T& operator*() & { return value(); }
  constexpr const T& operator*() const& { return value(); }
  constexpr T&& operator*() && { return std::move(*this).value(); }
  constexpr T* operator->() { return &value(); }
  constexpr const T* operator->() const { return &value(); }

  template <class U>
  constexpr T value_or(U&& fallback) const& {
    return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  constexpr void check_value() const {
    if (!has_value()) detail::bad_expected_access("value() called on an error");
  }
  constexpr void check_error() const {
    if (has_value()) detail::bad_expected_access("error() called on a value");
  }

  std::variant<T, E> storage_;
};

template <class E>
class Expected<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  constexpr Expected() noexcept = default;
  template <class G>
  constexpr Expected(const Unexpected<G>& u) : error_(u.error()), has_error_(true) {}  // NOLINT
  template <class G>
  constexpr Expected(Unexpected<G>&& u)  // NOLINT
      : error_(std::move(u).error()), has_error_(true) {}

  constexpr bool has_value() const noexcept { return !has_error_; }
  constexpr explicit operator bool() const noexcept { return has_value(); }

  constexpr void value() const {
    if (has_error_) detail::bad_expected_access("value() called on an error");
  }
  constexpr void operator*() const noexcept {}

  constexpr const E& error() const& {
    if (!has_error_) detail::bad_expected_access("error() called on a value");
    return error_;
  }
  constexpr E& error() & {
    if (!has_error_) detail::bad_expected_access("error() called on a value");
    return error_;
  }

 private:
  E error_{};
  bool has_error_ = false;
};

}  // namespace pychron

#endif
