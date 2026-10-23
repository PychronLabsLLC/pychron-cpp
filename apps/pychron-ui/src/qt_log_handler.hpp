#pragma once

#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"

namespace pychron::ui {

// Sends Qt's own messages (qDebug .. qFatal) to logger `qt` on `hub`
// (debug, info, warn, error, error), then to the handler that was installed
// before. The destructor restores that handler. A message from any thread is
// safe, and none waits for another's record to be written; a message that
// comes in while this object is destroyed may still be written (a hub logger
// writes nothing once its hub is gone). One instance at a time.
class QtLogHandler {
 public:
  explicit QtLogHandler(LogHub& hub);
  ~QtLogHandler();
  QtLogHandler(const QtLogHandler&) = delete;
  QtLogHandler& operator=(const QtLogHandler&) = delete;
};

}  // namespace pychron::ui
