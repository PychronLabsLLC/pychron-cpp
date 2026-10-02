// LogModel ring buffer and LogFilterProxy filtering.

#include <QtTest/QtTest>

#include "log_filter_proxy.hpp"
#include "log_model.hpp"

using namespace pychron;
using pychron::ui::LogFilterProxy;
using pychron::ui::LogModel;
using pychron::ui::LogRecord;

namespace {

LogRecord rec(LogLevel level, const char* logger, const char* msg, bool history = false) {
  return LogRecord{{}, level, QString::fromLatin1(logger), QString::fromLatin1(msg), history};
}

QStringList messages(const QAbstractItemModel& m) {
  QStringList out;
  for (int r = 0; r < m.rowCount(); ++r) out << m.index(r, 3).data().toString();
  return out;
}

}  // namespace

class TestLogModel : public QObject {
  Q_OBJECT

 private slots:
  void evictsOldestBeyondCapacity() {
    LogModel model(3);
    std::vector<LogRecord> batch;
    for (int i = 1; i <= 5; ++i) batch.push_back(rec(LogLevel::Info, "a", QByteArray::number(i).constData()));
    model.append(std::move(batch));
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(messages(model), (QStringList{"3", "4", "5"}));
    model.append({rec(LogLevel::Info, "a", "6")});
    QCOMPARE(messages(model), (QStringList{"4", "5", "6"}));
  }

  void clearEmpties() {
    LogModel model;
    model.append({rec(LogLevel::Info, "a", "x")});
    model.clear();
    QCOMPARE(model.rowCount(), 0);
  }

  void minLevelHidesBelow() {
    LogModel model;
    LogFilterProxy proxy;
    proxy.setSourceModel(&model);
    model.append({rec(LogLevel::Debug, "a", "d"), rec(LogLevel::Warn, "a", "w"), rec(LogLevel::Error, "a", "e")});
    proxy.set_min_level(LogLevel::Warn);
    QCOMPARE(messages(proxy), (QStringList{"w", "e"}));
    QCOMPARE(proxy.index(0, 1).data(Qt::UserRole).toInt(), static_cast<int>(LogLevel::Warn));
  }

  void loggerPatternBareNameIncludesChildren() {
    LogModel model;
    LogFilterProxy proxy;
    proxy.setSourceModel(&model);
    model.append({rec(LogLevel::Info, "gauge", "1"), rec(LogLevel::Info, "gauge.ig1", "2"),
                  rec(LogLevel::Info, "gaugex", "3"), rec(LogLevel::Info, "valve", "4")});
    proxy.set_logger_pattern("gauge");
    QCOMPARE(messages(proxy), (QStringList{"1", "2"}));
  }

  void loggerPatternGlob() {
    LogModel model;
    LogFilterProxy proxy;
    proxy.setSourceModel(&model);
    model.append({rec(LogLevel::Info, "a.wire", "1"), rec(LogLevel::Info, "b.wire", "2"),
                  rec(LogLevel::Info, "a.core", "3")});
    proxy.set_logger_pattern("*.wire");
    QCOMPARE(messages(proxy), (QStringList{"1", "2"}));
  }

  void textFilterIsCaseInsensitive() {
    LogModel model;
    LogFilterProxy proxy;
    proxy.setSourceModel(&model);
    model.append({rec(LogLevel::Info, "a", "Read FAILED"), rec(LogLevel::Info, "a", "ok")});
    proxy.set_text("failed");
    QCOMPARE(messages(proxy), (QStringList{"Read FAILED"}));
  }

  void filtersAreReversible() {
    LogModel model;
    LogFilterProxy proxy;
    proxy.setSourceModel(&model);
    model.append({rec(LogLevel::Debug, "a", "1"), rec(LogLevel::Error, "b", "2")});
    proxy.set_min_level(LogLevel::Error);
    proxy.set_logger_pattern("b");
    proxy.set_text("zzz");
    QCOMPARE(proxy.rowCount(), 0);
    proxy.set_min_level(LogLevel::Trace);
    proxy.set_logger_pattern("");
    proxy.set_text("");
    QCOMPARE(messages(proxy), (QStringList{"1", "2"}));
    QCOMPARE(model.rowCount(), 2);
  }

  void historyFlagPreserved() {
    LogModel model;
    model.append({rec(LogLevel::Info, "a", "old", true), rec(LogLevel::Info, "a", "new")});
    QVERIFY(model.record(0).history);
    QVERIFY(!model.record(1).history);
  }

  void messageWithNewlineStaysOneRow() {
    LogModel model;
    model.append({rec(LogLevel::Info, "a", "line1\nline2")});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.index(0, 3).data().toString(), QStringLiteral("line1\nline2"));
  }
};

QTEST_APPLESS_MAIN(TestLogModel)
#include "test_log_model.moc"
