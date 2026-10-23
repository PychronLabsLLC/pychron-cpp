#include "qt_log_handler.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

#include <QString>
#include <QtGlobal>

namespace pychron::ui {

namespace {

// Guards the instance and the previous handler: a message from a worker thread
// can arrive while the handler is being removed. Held only to copy them out:
// the record is written after (a synchronous error flush, bus handlers), and a
// record that itself produces a Qt message comes back in here unblocked.
std::mutex g_mutex;
std::shared_ptr<const Logger> g_logger;  // shared: a write in flight keeps it after the handler goes
QtMessageHandler g_previous = nullptr;

LogLevel level_of(QtMsgType type) noexcept {
  switch (type) {
    case QtDebugMsg: return LogLevel::Debug;
    case QtInfoMsg: return LogLevel::Info;
    case QtWarningMsg: return LogLevel::Warn;
    case QtCriticalMsg:
    case QtFatalMsg: return LogLevel::Error;
  }
  return LogLevel::Error;
}

void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  QtMessageHandler previous = nullptr;
  std::shared_ptr<const Logger> logger;
  {
    const std::lock_guard lock(g_mutex);
    logger = g_logger;
    previous = g_previous;
  }
  if (logger != nullptr) logger->log(level_of(type), message.toStdString());
  if (previous != nullptr) {
    previous(type, context, message);
    return;
  }
  // No handler before ours: Qt's default prints to stderr and aborts on a fatal one.
  const QString line = qFormatLogMessage(type, context, message);
  std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
  std::fflush(stderr);
  if (type == QtFatalMsg) std::abort();
}

}  // namespace

QtLogHandler::QtLogHandler(LogHub& hub) {
  auto logger = std::make_shared<const Logger>(hub.logger("qt"));
  const std::lock_guard lock(g_mutex);
  g_logger = std::move(logger);
  g_previous = qInstallMessageHandler(handle);
}

QtLogHandler::~QtLogHandler() {
  std::shared_ptr<const Logger> logger;
  {
    const std::lock_guard lock(g_mutex);
    qInstallMessageHandler(g_previous);
    logger = std::move(g_logger);
    g_previous = nullptr;
  }
  // `logger` goes here, outside the lock.
}

}  // namespace pychron::ui
