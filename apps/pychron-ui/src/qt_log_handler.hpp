#pragma once

#include "pychron/core/log_hub.hpp"
#include "pychron/core/logger.hpp"

namespace pychron::ui {

// Sends Qt's own messages (qDebug .. qFatal) to logger `qt` on `hub`
// (debug, info, warn, error, error), then to the handler that was installed
// before. The destructor restores that handler. A message from any thread is
// safe; the hub must outlive this object. One instance at a time.
class QtLogHandler {
 public:
  explicit QtLogHandler(LogHub& hub);
  ~QtLogHandler();
  QtLogHandler(const QtLogHandler&) = delete;
  QtLogHandler& operator=(const QtLogHandler&) = delete;

 private:
  Logger logger_;
};

}  // namespace pychron::ui
