#include "pychron/core/log_context.hpp"

#include <atomic>
#include <utility>

namespace pychron::LogContext {
namespace {

// Read and written only by the owning thread.
thread_local std::string t_tag;
thread_local std::string t_run;
thread_local std::uint32_t t_number = 0;

std::atomic<std::uint32_t> g_next_number{1};

}  // namespace

Thread::Thread(std::string tag) : previous_(std::exchange(t_tag, std::move(tag))) {}
Thread::~Thread() { t_tag = std::move(previous_); }

Run::Run(std::string run_id) : previous_(std::exchange(t_run, std::move(run_id))) {}
Run::~Run() { t_run = std::move(previous_); }

std::string_view thread_tag() noexcept { return t_tag; }
std::string_view run_id() noexcept { return t_run; }

std::uint32_t thread_number() noexcept {
  if (t_number == 0) t_number = g_next_number.fetch_add(1, std::memory_order_relaxed);
  return t_number;
}

}  // namespace pychron::LogContext
