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
    const auto hub = pychron::ui::open_process_log(install, std::nullopt, clock, false, pychron::HostOs::Mac, env).hub;
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
    const auto opened = pychron::ui::open_process_log(std::nullopt, std::nullopt, clock, true, pychron::HostOs::Mac,
                                                      env, "virtual x10");
    QVERIFY(opened.notices.empty());
    const auto& hub = opened.hub;
    QVERIFY(hub != nullptr);
    hub->flush();
    const fs::path expected = fs::path(tmp.path().toStdString()) / "Library" / "Logs" / "pychron" / "pychron.log";
    QVERIFY(fs::exists(expected));
    QVERIFY(slurp(expected).find("virtual x10") != std::string::npos);
  }

  // A logging.toml that does not load: the built-in defaults in the install's
  // folder, the reason as the first error record (logger `logging`) and as a
  // notice for the Log dock.
  void badLoggingTomlFallsBackToDefaults() {
    QTemporaryDir tmp;
    pychron::setup::SiteInstall install;
    install.name = "lab";
    install.kind = "instrument";
    install.root = fs::path(tmp.path().toStdString());
    {
      std::ofstream out(install.root / "logging.toml");
      out << "colour = 1\n";
    }
    const pychron::EnvLookup env = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    pychron::SteadyClock clock;
    const auto opened = pychron::ui::open_process_log(install, std::nullopt, clock, false, pychron::HostOs::Mac, env);
    QVERIFY(opened.hub != nullptr);
    QCOMPARE(opened.hub->file_path(), install.root / "logs" / "pychron.log");
    QCOMPARE(opened.notices.size(), std::size_t{1});
    QCOMPARE(opened.notices[0].first, pychron::LogLevel::Error);
    QVERIFY2(opened.notices[0].second.find("colour") != std::string::npos, opened.notices[0].second.c_str());
    opened.hub->flush();
    const std::string text = slurp(opened.hub->file_path());
    const auto at = text.find("[error]");
    QVERIFY2(at != std::string::npos, text.c_str());
    const std::string first_error = text.substr(at, text.find('\n', at) - at);
    QVERIFY2(first_error.find("] logging: ") != std::string::npos, first_error.c_str());
    QVERIFY2(first_error.find("colour") != std::string::npos, first_error.c_str());
    QVERIFY2(text.find("default_level=debug") != std::string::npos, text.c_str());
  }

  // No HOME and no install: no folder for the file, said once.
  void noHomeIsANoticeAndNoFile() {
    const pychron::EnvLookup env = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    pychron::SteadyClock clock;
    const auto opened =
        pychron::ui::open_process_log(std::nullopt, std::nullopt, clock, false, pychron::HostOs::Linux, env);
    QVERIFY(opened.hub != nullptr);
    QVERIFY(opened.hub->file_path().empty());
    QCOMPARE(opened.notices.size(), std::size_t{1});
    QCOMPARE(opened.notices[0].first, pychron::LogLevel::Warn);
    QCOMPARE(opened.notices[0].second, std::string("no log folder: HOME is not set"));
  }
};

QTEST_MAIN(TestProcessLog)
#include "test_process_log.moc"
