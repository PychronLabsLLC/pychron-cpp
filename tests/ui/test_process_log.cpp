// The process log hub of pychron-ui: made before any window, line or no line,
// and Qt's own messages go to logger `qt`.

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "pychron/core/clock.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/setup/site.hpp"
#include "process_log.hpp"
#include "qt_log_handler.hpp"

namespace fs = std::filesystem;

class TestProcessLog : public QObject {
  Q_OBJECT

  static std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

 private slots:
  void qtWarningReachesFile() {
    QTemporaryDir tmp;
    const fs::path root = tmp.path().toStdString();
    pychron::SteadyClock clock;
    pychron::config::LoggingConfig cfg;
    cfg.dir = root / "logs";
    auto created = pychron::LogHub::create(cfg, clock);
    QVERIFY(created.has_value());
    const auto hub = std::move(*created);
    {
      pychron::ui::QtLogHandler handler(*hub);
      qWarning("boom");
    }
    hub->flush();
    const std::string text = slurp(hub->file_path());
    QVERIFY2(text.find("[warn]") != std::string::npos, text.c_str());
    QVERIFY2(text.find("qt: boom") != std::string::npos, text.c_str());
  }

  void handlerRestoresThePreviousHandlerAndChains() {
    static int seen = 0;
    seen = 0;
    const auto previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) { ++seen; });
    QTemporaryDir tmp;
    pychron::SteadyClock clock;
    pychron::config::LoggingConfig cfg;
    cfg.dir = fs::path(tmp.path().toStdString()) / "logs";
    const auto hub = *pychron::LogHub::create(cfg, clock);
    {
      pychron::ui::QtLogHandler handler(*hub);
      qInfo("chained");
      QCOMPARE(seen, 1);
    }
    qInfo("restored");
    QCOMPARE(seen, 2);
    qInstallMessageHandler(previous);
  }

  void dataReductionInstallLogs() {
    QTemporaryDir tmp;
    pychron::setup::SiteInstall install;
    install.name = "dr";
    install.kind = "data_reduction";
    install.root = fs::path(tmp.path().toStdString());
    const pychron::EnvLookup env = [&](std::string_view name) -> std::optional<std::string> {
      if (name == "HOME") return tmp.path().toStdString();
      return std::nullopt;
    };
    pychron::SteadyClock clock;
    const auto hub = pychron::ui::open_process_log(install, std::nullopt, clock, false, pychron::HostOs::Mac, env);
    QVERIFY(hub != nullptr);
    hub->flush();
    QVERIFY(fs::exists(install.root / "logs" / "pychron.log"));
    QCOMPARE(hub->file_path(), install.root / "logs" / "pychron.log");
  }

  void noInstallUsesTheOsFolderUnderTheInjectedHome() {
    QTemporaryDir tmp;
    const pychron::EnvLookup env = [&](std::string_view name) -> std::optional<std::string> {
      if (name == "HOME") return tmp.path().toStdString();
      return std::nullopt;
    };
    pychron::SteadyClock clock;
    const auto hub = pychron::ui::open_process_log(std::nullopt, std::nullopt, clock, true, pychron::HostOs::Mac, env,
                                                   "virtual x10");
    QVERIFY(hub != nullptr);
    hub->flush();
    const fs::path expected = fs::path(tmp.path().toStdString()) / "Library" / "Logs" / "pychron" / "pychron.log";
    QVERIFY(fs::exists(expected));
    QVERIFY(slurp(expected).find("virtual x10") != std::string::npos);
  }
};

QTEST_MAIN(TestProcessLog)
#include "test_process_log.moc"
