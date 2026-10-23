#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Per-thread context stamped on every log record: a thread tag ("executor",
// "scanner") and, inside a run scope, the run id. Both are read on the thread
// that logs, when the record is made; nothing here is shared between threads
// apart from the counter behind thread_number().
namespace pychron::LogContext {

// Names the current thread for the life of the scope; the previous tag comes
// back when it ends.
class Thread {
 public:
  explicit Thread(std::string tag);
  ~Thread();
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(Thread&&) = delete;

 private:
  std::string value_;
  const std::string* previous_;
};

// Marks the records of the current thread as part of a run. Scopes nest: an
// inner one replaces the id, and its end restores the outer one.
class Run {
 public:
  explicit Run(std::string run_id);
  ~Run();
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;
  Run(Run&&) = delete;
  Run& operator=(Run&&) = delete;

 private:
  std::string value_;
  const std::string* previous_;
};

// The current thread's tag; empty when no Thread scope is open.
[[nodiscard]] std::string_view thread_tag() noexcept;
// The current thread's run id; empty outside a Run scope.
[[nodiscard]] std::string_view run_id() noexcept;
// A small number for the current thread, assigned on its first call, from 1.
[[nodiscard]] std::uint32_t thread_number() noexcept;

}  // namespace pychron::LogContext
