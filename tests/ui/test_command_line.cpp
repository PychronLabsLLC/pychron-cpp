// pychron-ui argument parsing: positional files, --sim and --spectrometer.

#include <filesystem>

#include <QtTest/QtTest>

#include "command_line.hpp"

using pychron::ui::parse_command_line;
using Path = std::filesystem::path;

class TestCommandLine : public QObject {
  Q_OBJECT

 private slots:
  void noArgumentsIsDefaults() {
    auto cli = parse_command_line({});
    QVERIFY(cli.has_value());
    QVERIFY(!cli->sim);
    QVERIFY(cli->files.empty());
    QVERIFY(!cli->spectrometer_file.has_value());
  }

  void bareSpectrometerIsUsageError() {
    auto cli = parse_command_line({QStringLiteral("--spectrometer")});
    QVERIFY(!cli.has_value());
    QCOMPARE(QString::fromStdString(cli.error().what), QStringLiteral("--spectrometer needs a file"));
  }

  void spectrometerFollowedByFlagIsUsageError() {
    auto cli = parse_command_line({QStringLiteral("--spectrometer"), QStringLiteral("--sim")});
    QVERIFY(!cli.has_value());
    QCOMPARE(cli.error().kind, pychron::ErrorKind::Config);
  }

  void spectrometerFileThenSim() {
    auto cli = parse_command_line({QStringLiteral("--spectrometer"), QStringLiteral("f.toml"), QStringLiteral("--sim")});
    QVERIFY(cli.has_value());
    QVERIFY(cli->sim);
    QVERIFY(cli->spectrometer_file == Path("f.toml"));
    QVERIFY(cli->files.empty());
  }

  void positionalFilesMixWithFlags() {
    auto cli = parse_command_line({QStringLiteral("line.toml"), QStringLiteral("--sim"), QStringLiteral("canvas.toml"),
                                   QStringLiteral("--spectrometer"), QStringLiteral("spec.toml")});
    QVERIFY(cli.has_value());
    QVERIFY(cli->sim);
    QCOMPARE(cli->files.size(), std::size_t{2});
    QVERIFY(cli->files[0] == Path("line.toml"));
    QVERIFY(cli->files[1] == Path("canvas.toml"));
    QVERIFY(cli->spectrometer_file == Path("spec.toml"));
  }
};

QTEST_APPLESS_MAIN(TestCommandLine)
#include "test_command_line.moc"
