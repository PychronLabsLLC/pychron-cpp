#include "pychron/core/process_log.hpp"

#include <mutex>
#include <utility>

namespace pychron {

namespace {

std::mutex& hub_mutex() {
  static std::mutex m;
  return m;
}

std::shared_ptr<LogHub>& hub_slot() {
  static std::shared_ptr<LogHub> hub;
  return hub;
}

}  // namespace

std::shared_ptr<LogHub> process_log_hub() {
  std::lock_guard lock(hub_mutex());
  return hub_slot();
}

void set_process_log_hub(std::shared_ptr<LogHub> hub) {
  std::shared_ptr<LogHub> old;
  {
    std::lock_guard lock(hub_mutex());
    old = std::move(hub_slot());
    hub_slot() = std::move(hub);
  }
  // `old` is destroyed here, outside the lock: its destructor flushes.
}

Logger process_logger(std::string name) {
  if (auto hub = process_log_hub()) return hub->logger(std::move(name));
  return Logger::disabled(std::move(name));
}

ProcessLogScope::ProcessLogScope(std::shared_ptr<LogHub> hub) : previous_(process_log_hub()) {
  set_process_log_hub(std::move(hub));
}

ProcessLogScope::~ProcessLogScope() { set_process_log_hub(std::move(previous_)); }

}  // namespace pychron
