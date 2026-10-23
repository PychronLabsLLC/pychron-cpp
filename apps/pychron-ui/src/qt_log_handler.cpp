#include "qt_log_handler.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <QString>
#include <QtGlobal>

namespace pychron::ui {

namespace {

// Guards the instance and the previous handler: a message from a worker thread
// can arrive while the handler is being removed. Recursive, since writing a
// record could itself produce a Qt message.
std::recursive_mutex g_mutex;
const Logger* g_logger = nullptr;
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
  {
    const std::lock_guard lock(g_mutex);
    if (g_logger != nullptr) g_logger->log(level_of(type), message.toStdString());
    previous = g_previous;
  }
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

QtLogHandler::QtLogHandler(LogHub& hub) : logger_(hub.logger("qt")) {
  const std::lock_guard lock(g_mutex);
  g_logger = &logger_;
  g_previous = qInstallMessageHandler(handle);
}

QtLogHandler::~QtLogHandler() {
  const std::lock_guard lock(g_mutex);
  qInstallMessageHandler(g_previous);
  g_logger = nullptr;
  g_previous = nullptr;
}

}  // namespace pychron::ui
