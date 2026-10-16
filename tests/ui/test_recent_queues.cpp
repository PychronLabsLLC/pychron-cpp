// RecentQueues: the experiment window's File > Open Recent list, on a
// QSettings ini file in a temporary directory.

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "recent_queues.hpp"

using pychron::ui::RecentQueues;
namespace fs = std::filesystem;

class TestRecentQueues : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<QSettings> settings_;

  fs::path dir() const { return fs::path(dir_->path().toStdString()); }

  // A queue file in the directory, made on disk.
  fs::path touch(const std::string& relative) const {
    const fs::path p = dir() / relative;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "# queue\n";
    return p;
  }
  // What the list holds for `p`: absolute, with links resolved.
  static QString entry(const fs::path& p) { return QString::fromStdString(fs::weakly_canonical(p).string()); }

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    settings_ = std::make_unique<QSettings>(dir_->filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
  }
  void cleanup() {
    settings_.reset();
    dir_.reset();
  }

  void newestFirst() {
    RecentQueues recent(*settings_);
    const fs::path a = touch("a.toml");
    const fs::path b = touch("b.toml");
    recent.add(a);
    recent.add(b);
    QCOMPARE(recent.entries(), (QStringList{entry(b), entry(a)}));
    QVERIFY(fs::path(recent.entries().front().toStdString()).is_absolute());
  }

  void aReopenedQueueMovesToTheTop() {
    RecentQueues recent(*settings_);
    const fs::path a = touch("a.toml");
    const fs::path b = touch("b.toml");
    recent.add(a);
    recent.add(b);
    recent.add(a);
    QCOMPARE(recent.entries(), (QStringList{entry(a), entry(b)}));
  }

  void keepsTen() {
    RecentQueues recent(*settings_);
    QStringList added;
    for (int i = 1; i <= 12; ++i) {
      const fs::path p = touch("q" + std::to_string(i) + ".toml");
      recent.add(p);
      added.append(entry(p));
    }
    const QStringList entries = recent.entries();
    QCOMPARE(entries.size(), RecentQueues::kMax);
    QCOMPARE(entries.front(), added.at(11));
    QVERIFY(!entries.contains(added.at(0)));
    QVERIFY(!entries.contains(added.at(1)));
  }

  void oneEntryForTwoSpellings() {
    RecentQueues recent(*settings_);
    const fs::path x = touch("queues/x.toml");
    recent.add(x);
    recent.add(dir() / "queues" / ".." / "queues" / "x.toml");
    QCOMPARE(recent.entries(), QStringList{entry(x)});
  }

  void aDeletedQueueIsLeftOutAndForgotten() {
    RecentQueues recent(*settings_);
    const fs::path a = touch("a.toml");
    const fs::path b = touch("b.toml");
    recent.add(a);
    recent.add(b);
    fs::remove(a);
    QCOMPARE(recent.entries(), QStringList{entry(b)});
    QCOMPARE(settings_->value(QLatin1String(RecentQueues::kKey)).toStringList(), QStringList{entry(b)});
  }

  void clearEmptiesIt() {
    RecentQueues recent(*settings_);
    recent.add(touch("a.toml"));
    recent.clear();
    QVERIFY(recent.entries().isEmpty());
    QVERIFY(!settings_->contains(QLatin1String(RecentQueues::kKey)));
  }
};

QTEST_MAIN(TestRecentQueues)
#include "test_recent_queues.moc"
