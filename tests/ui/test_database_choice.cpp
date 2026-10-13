// Which database pychron-ui uses: --db, else the install's with its password.

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "command_line.hpp"
#include "database_choice.hpp"
#include "pychron/setup/site.hpp"

using pychron::ui::choose_database;
using pychron::ui::CommandLine;
namespace setup = pychron::setup;

class TestDatabaseChoice : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;

  setup::SiteInstall install(const std::string& database) const {
    setup::SiteInstall i;
    i.name = "argus";
    i.kind = "instrument";
    i.root = std::filesystem::path(tmp_.path().toStdString());
    i.database = database;
    return i;
  }
  void write_credentials(const std::string& text) const {
    const auto dir = std::filesystem::path(tmp_.path().toStdString()) / ".pychron";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "credentials.toml") << text;
  }

 private slots:
  void cleanup() { std::filesystem::remove_all(std::filesystem::path(tmp_.path().toStdString()) / ".pychron"); }

  void dbWinsOverTheInstall() {
    CommandLine cli;
    cli.db = "sqlite:/elsewhere/other.db";
    const auto i = install("sqlite:/lab/data/pychron.db");
    const auto url = choose_database(cli, &i);
    QVERIFY(url.has_value());
    QCOMPARE(QString::fromStdString(url->value_or("")), QStringLiteral("sqlite:/elsewhere/other.db"));
    // ... and is not held up by an install whose password cannot be read.
    write_credentials("[database\n");
    const auto server = install("postgresql://lab@db:5432/pychron");
    QVERIFY(choose_database(cli, &server).has_value());
  }

  void theInstallsDatabaseIsUsedWithoutDb() {
    const auto local = install("sqlite:/lab/data/pychron.db");
    const auto url = choose_database(CommandLine{}, &local);
    QVERIFY(url.has_value());
    QCOMPARE(QString::fromStdString(url->value_or("")), QStringLiteral("sqlite:/lab/data/pychron.db"));
    // A server's password comes from the install.
    write_credentials("[database]\npassword = \"s3 cret\"\n");
    const auto server = install("postgresql://lab@db:5432/pychron");
    const auto with_password = choose_database(CommandLine{}, &server);
    QVERIFY(with_password.has_value());
    QCOMPARE(QString::fromStdString(with_password->value_or("")),
             QStringLiteral("postgresql://lab:s3%20cret@db:5432/pychron"));
  }

  void noneWithoutEither() {
    const auto url = choose_database(CommandLine{}, nullptr);
    QVERIFY(url.has_value());
    QVERIFY(!url->has_value());
  }

  void anInstallWithNoDatabaseHasNone() {
    const auto i = install("");
    const auto url = choose_database(CommandLine{}, &i);
    QVERIFY(url.has_value());
    QVERIFY(!url->has_value());
  }

  void anInstallWhosePasswordCannotBeReadIsAnError() {
    write_credentials("[database\n");
    const auto server = install("postgresql://lab@db:5432/pychron");
    const auto url = choose_database(CommandLine{}, &server);
    QVERIFY(!url.has_value());
    QVERIFY(QString::fromStdString(url.error().what).contains(QStringLiteral("credentials.toml")));
  }
#ifdef PYCHRON_UI_HAS_STORE
  // A simulated session saves to the database only when it is the install's
  // own file here, or with --sim-save; otherwise it says, once, where its
  // runs go instead. A session that is not simulated always saves.
  void aSimulatedSessionSavesOnlyToItsInstallsLocalDatabaseUnlessAsked() {
    using pychron::ui::choose_run_saving;
    const std::filesystem::path data = "/lab/data";
    const std::string local = "sqlite:/lab/data/pychron.db";
    const std::string server = "postgresql://lab:s3cret@db:5432/pychron";

    QVERIFY(choose_run_saving(CommandLine{}, local, true, data).to_database);
    QVERIFY(choose_run_saving(CommandLine{}, local, true, data).note.empty());

    const auto shared = choose_run_saving(CommandLine{}, server, true, data);
    QVERIFY(!shared.to_database);
    QCOMPARE(QString::fromStdString(shared.note),
             QStringLiteral("simulated runs are written to /lab/data/records, not to postgresql://lab@db:5432/pychron; "
                            "pass --sim-save to save them there"));

    CommandLine pointed;
    pointed.db = local;  // even a file, when it was given with --db
    const auto given = choose_run_saving(pointed, local, true, data);
    QVERIFY(!given.to_database);
    QVERIFY(QString::fromStdString(given.note).contains(QStringLiteral("not to sqlite:/lab/data/pychron.db;")));

    pointed.sim_save = true;
    QVERIFY(choose_run_saving(pointed, local, true, data).to_database);
    CommandLine asked;
    asked.sim_save = true;
    QVERIFY(choose_run_saving(asked, server, true, data).to_database);

    // An instrument: wherever its database is.
    QVERIFY(choose_run_saving(CommandLine{}, server, false, data).to_database);
    QVERIFY(choose_run_saving(pointed, local, false, data).to_database);
    CommandLine db_only;
    db_only.db = server;
    QVERIFY(choose_run_saving(db_only, server, false, data).to_database);
    QVERIFY(choose_run_saving(db_only, server, false, data).note.empty());
  }
#endif
};

QTEST_APPLESS_MAIN(TestDatabaseChoice)
#include "test_database_choice.moc"
