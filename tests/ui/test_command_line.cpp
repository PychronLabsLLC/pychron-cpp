// pychron-ui argument parsing: positional files, --sim, --spectrometer and the
// experiment options.

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

  void experimentOptions() {
    auto cli = parse_command_line({QStringLiteral("--sim"), QStringLiteral("--lab"), QStringLiteral("lab"),
                                   QStringLiteral("--data"), QStringLiteral("out"), QStringLiteral("--queue"),
                                   QStringLiteral("q.toml"), QStringLiteral("--sim-speed"), QStringLiteral("400")});
    QVERIFY(cli.has_value());
    QVERIFY(cli->lab == Path("lab"));
    QVERIFY(cli->data == Path("out"));
    QVERIFY(cli->queue == Path("q.toml"));
    QCOMPARE(cli->sim_speed, 400.0);
    QVERIFY(cli->files.empty());
    auto defaults = parse_command_line({});
    QVERIFY(!defaults->lab && !defaults->data && !defaults->queue);
    QCOMPARE(defaults->sim_speed, 0.0);
  }

  void experimentUsageErrors() {
    QCOMPARE(QString::fromStdString(parse_command_line({QStringLiteral("--lab")}).error().what),
             QStringLiteral("--lab needs a directory"));
    QCOMPARE(QString::fromStdString(parse_command_line({QStringLiteral("--queue"), QStringLiteral("--sim")}).error().what),
             QStringLiteral("--queue needs a file"));
    for (const char* bad : {"0", "-2", "fast", "inf"}) {
      auto cli = parse_command_line({QStringLiteral("--sim"), QStringLiteral("--sim-speed"), QString::fromLatin1(bad)});
      QVERIFY2(!cli.has_value(), bad);
    }
    QVERIFY(!parse_command_line({QStringLiteral("--sim"), QStringLiteral("--sim-speed")}).has_value());
    auto no_sim = parse_command_line({QStringLiteral("--sim-speed"), QStringLiteral("10")});
    QVERIFY(!no_sim.has_value());
    QCOMPARE(QString::fromStdString(no_sim.error().what), QStringLiteral("--sim-speed needs --sim"));
  }
};

QTEST_APPLESS_MAIN(TestCommandLine)
#include "test_command_line.moc"
