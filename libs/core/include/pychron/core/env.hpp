#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace pychron {

// Value of environment variable `name`, or nullopt when it is unset.
// MSVC deprecates std::getenv (C4996); _dupenv_s is its replacement there.
inline std::optional<std::string> env_var(const char* name) {
#ifdef _MSC_VER
  char* raw = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&raw, &size, name) != 0 || raw == nullptr) return std::nullopt;
  std::string value(raw);
  std::free(raw);
  return value;
#else
  // NOLINTNEXTLINE(concurrency-mt-unsafe): the one place the environment is read; only tests and start-up set it
  const char* raw = std::getenv(name);
  if (raw == nullptr) return std::nullopt;
  return std::string(raw);
#endif
}

}  // namespace pychron
