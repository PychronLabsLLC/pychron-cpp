#include "pychron/core/log_context.hpp"

#include <atomic>
#include <string>
#include <type_traits>
#include <utility>

namespace pychron::LogContext {
namespace {

// Read and written only by the owning thread. Pointers to the innermost open
// scope's string, so nothing here has a destructor: a thread that logs while
// it exits, after other thread_locals are gone, never reads a destroyed string.
thread_local const std::string* t_tag = nullptr;
thread_local const std::string* t_run = nullptr;
thread_local std::uint32_t t_number = 0;

static_assert(std::is_trivially_destructible_v<decltype(t_tag)>);
static_assert(std::is_trivially_destructible_v<decltype(t_run)>);
static_assert(std::is_trivially_destructible_v<decltype(t_number)>);

std::atomic<std::uint32_t> g_next_number{1};

}  // namespace

Thread::Thread(std::string tag) : value_(std::move(tag)), previous_(std::exchange(t_tag, &value_)) {}
Thread::~Thread() { t_tag = previous_; }

Run::Run(std::string run_id) : value_(std::move(run_id)), previous_(std::exchange(t_run, &value_)) {}
Run::~Run() { t_run = previous_; }

std::string_view thread_tag() noexcept { return t_tag != nullptr ? std::string_view{*t_tag} : std::string_view{}; }
std::string_view run_id() noexcept { return t_run != nullptr ? std::string_view{*t_run} : std::string_view{}; }

std::uint32_t thread_number() noexcept {
  if (t_number == 0) t_number = g_next_number.fetch_add(1, std::memory_order_relaxed);
  return t_number;
}

}  // namespace pychron::LogContext
