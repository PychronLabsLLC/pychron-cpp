#pragma once

// The log hub of this process: made by main() of an app, read by whatever
// wants to log without being handed a hub. Tests that call an app's run()
// directly have none, and nothing they do writes to the user's folder.

#include <memory>
#include <string>

#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"

namespace pychron {

// The hub main() made; null when there is none.
std::shared_ptr<LogHub> process_log_hub();

// main() and tests; thread-safe.
void set_process_log_hub(std::shared_ptr<LogHub> hub);

// A logger on the process hub, or one that writes nothing when there is none
// (Logger::disabled: no hub, no bus; enabled() costs one atomic load).
Logger process_logger(std::string name);

// Tests: sets the process hub for the scope, restores the previous one after.
class ProcessLogScope {
 public:
  explicit ProcessLogScope(std::shared_ptr<LogHub> hub);
  ~ProcessLogScope();
  ProcessLogScope(const ProcessLogScope&) = delete;
  ProcessLogScope& operator=(const ProcessLogScope&) = delete;

 private:
  std::shared_ptr<LogHub> previous_;
};

}  // namespace pychron
