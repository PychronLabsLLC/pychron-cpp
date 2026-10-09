// DockLayouts: a window's factory layout, its last one and its named
// arrangements, on a bare QMainWindow with three docks.

#include <functional>
#include <memory>
#include <string>

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "dock_layouts.hpp"

using pychron::ErrorKind;
using pychron::ui::DockLayouts;

namespace {

// A (left), B (right), C (right, behind B, not closable); with `extra`, D
// (bottom) as well.
struct Window {
  explicit Window(QSettings* settings, bool extra = false, DockLayouts::Keys keys = {}) {
    w.setCentralWidget(new QLabel(QStringLiteral("center")));
    a = dock(QStringLiteral("A"));
    b = dock(QStringLiteral("B"));
    c = dock(QStringLiteral("C"));
    c->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    if (extra) d = dock(QStringLiteral("D"));
    const std::function<void()> factory = [this] {
      for (QDockWidget* each : {a, b, c, d})
        if (each != nullptr) each->setFloating(false);
      w.addDockWidget(Qt::LeftDockWidgetArea, a);
      w.addDockWidget(Qt::RightDockWidgetArea, b);
      w.addDockWidget(Qt::RightDockWidgetArea, c);
      if (d != nullptr) w.addDockWidget(Qt::BottomDockWidgetArea, d);
      w.tabifyDockWidget(b, c);
      for (QDockWidget* each : {a, b, c, d})
        if (each != nullptr) each->show();
      b->raise();
    };
    factory();
    layouts = new DockLayouts(&w, factory, settings, QStringLiteral("win"), std::move(keys));
    w.resize(800, 600);
  }
  Q_DISABLE_COPY_MOVE(Window)
  ~Window() = default;

  QDockWidget* dock(const QString& name) {
    auto* made = new QDockWidget(name, &w);
    made->setObjectName(name);
    made->setWidget(new QLabel(name));
    return made;
  }

  void show() {
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
  }

  // Every dock docked, shown (B and C share tabs: one of them is in front),
  // in the place the factory gives it.
  void verify_factory() const {
    for (QDockWidget* each : {a, b, c, d}) {
      if (each == nullptr) continue;
      QVERIFY2(!each->isHidden(), qPrintable(each->objectName()));
      QVERIFY2(!each->isFloating(), qPrintable(each->objectName()));
    }
    QVERIFY(a->isVisible());
    QVERIFY(b->isVisible());
    QCOMPARE(w.dockWidgetArea(a), Qt::LeftDockWidgetArea);
    QCOMPARE(w.dockWidgetArea(b), Qt::RightDockWidgetArea);
    QCOMPARE(w.dockWidgetArea(c), Qt::RightDockWidgetArea);
    QVERIFY(w.tabifiedDockWidgets(b).contains(c));
  }

  QMainWindow w;
  QDockWidget* a = nullptr;
  QDockWidget* b = nullptr;
  QDockWidget* c = nullptr;
  QDockWidget* d = nullptr;
  DockLayouts* layouts = nullptr;
};

QStringList texts(const QList<QAction*>& actions) {
  QStringList out;
  for (const QAction* action : actions) out.append(action->text());
  return out;
}

}  // namespace

class TestDockLayouts : public QObject {
  Q_OBJECT

 private:
  static std::unique_ptr<QSettings> ini(const QTemporaryDir& dir) {
    return std::make_unique<QSettings>(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
  }

 private slots:
  void resetRestoresFactoryAfterEverythingMoved() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->close();
    win.b->setFloating(true);
    win.w.addDockWidget(Qt::BottomDockWidgetArea, win.c);
    QVERIFY(!win.a->isVisible());
    QVERIFY(win.b->isFloating());
    QCOMPARE(win.w.dockWidgetArea(win.c), Qt::BottomDockWidgetArea);

    win.layouts->reset();
    win.verify_factory();
  }

  void resetDocksPanelsThatFloatTogetherOrAreHiddenAndFloating() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->setFloating(true);
    win.b->setFloating(true);
    win.b->hide();

    win.layouts->reset();
    win.verify_factory();
    QCOMPARE(win.w.findChildren<QDockWidget*>().size(), 3);
  }

  void lastLayoutSurvivesASecondWindow() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    {
      Window first(settings.get());
      first.show();
      first.a->close();
      first.b->setFloating(true);
      first.layouts->save_last();
    }
    Window second(settings.get());
    second.layouts->restore_last();
    second.show();
    QVERIFY(!second.a->isVisible());
    QVERIFY(second.b->isFloating());
    QVERIFY(second.c->isVisible());
  }

  void garbageLastLayoutGivesFactoryWithoutError() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    settings->setValue(QStringLiteral("win/state"), QByteArray("not a layout"));
    Window win(settings.get());
    win.show();
    win.a->close();
    win.layouts->restore_last();
    win.verify_factory();
  }

  void customKeysAreUsed() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get(), false, DockLayouts::Keys{QStringLiteral("dock_state"), QStringLiteral("geometry")});
    win.show();
    win.layouts->save_last();
    QVERIFY(settings->contains(QStringLiteral("win/dock_state")));
    QVERIFY(settings->contains(QStringLiteral("win/geometry")));
    QVERIFY(!settings->contains(QStringLiteral("win/state")));
  }

  void withoutSettingsNothingIsWritten() {
    QTemporaryDir dir;
    Window win(nullptr);
    win.show();
    QVERIFY(!win.layouts->can_save());
    win.a->close();
    win.layouts->save_last();
    win.layouts->restore_last();
    QVERIFY(!win.a->isVisible());  // nothing saved, nothing restored
    win.layouts->reset();
    win.verify_factory();
    QVERIFY(QDir(dir.path()).isEmpty());
  }

  void withSettingsItCanSave() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    const Window win(settings.get());
    QVERIFY(win.layouts->can_save());
    QCOMPARE(win.layouts->window(), &win.w);
  }

  void panelActionsAreTheClosableDocks() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    const QList<QAction*> panels = win.layouts->panel_actions();
    QCOMPARE(texts(panels), QStringList({QStringLiteral("A"), QStringLiteral("B")}));  // C is not closable
    panels[0]->trigger();
    QVERIFY(!win.a->isVisible());
    panels[0]->trigger();
    QVERIFY(win.a->isVisible());
  }

  void ofFindsTheHelperOrNull() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    const Window win(settings.get());
    QCOMPARE(DockLayouts::of(&win.w), win.layouts);
    const QMainWindow bare;
    QVERIFY(DockLayouts::of(&bare) == nullptr);
    QVERIFY(DockLayouts::of(nullptr) == nullptr);
  }
  // ---- named arrangements -----------------------------------------------------

  void validNameTable_data() {
    QTest::addColumn<QString>("raw");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<QString>("trimmed");
    QTest::newRow("empty") << QString() << false << QString();
    QTest::newRow("spaces") << QStringLiteral("   ") << false << QString();
    QTest::newRow("65 characters") << QString(65, QLatin1Char('x')) << false << QString();
    QTest::newRow("64 characters") << QString(64, QLatin1Char('x')) << true << QString(64, QLatin1Char('x'));
    QTest::newRow("slash") << QStringLiteral("a/b") << false << QString();
    QTest::newRow("backslash") << QStringLiteral("a\\b") << false << QString();
    QTest::newRow("tab") << QStringLiteral("a\tb") << false << QString();
    QTest::newRow("trimmed") << QStringLiteral(" ok ") << true << QStringLiteral("ok");
  }

  void validNameTable() {
    QFETCH(QString, raw);
    QFETCH(bool, ok);
    QFETCH(QString, trimmed);
    const auto name = DockLayouts::valid_name(raw);
    QCOMPARE(name.has_value(), ok);
    if (ok) {
      QCOMPARE(*name, trimmed);
    } else {
      QCOMPARE(name.error().kind, ErrorKind::Config);
      QCOMPARE(name.error().code, std::string("bad_name"));
      QVERIFY(!name.error().what.empty());
    }
  }

  void arrangementRoundTrip() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->close();
    win.b->setFloating(true);
    QVERIFY(win.layouts->save_as(QStringLiteral("bakeout")));
    win.layouts->reset();
    win.verify_factory();
    QCOMPARE(win.layouts->names(), QStringList{QStringLiteral("bakeout")});

    QVERIFY(win.layouts->apply(QStringLiteral("bakeout")));
    QVERIFY(!win.a->isVisible());
    QVERIFY(win.b->isFloating());

    win.layouts->remove(QStringLiteral("bakeout"));
    QVERIFY(win.layouts->names().isEmpty());
    QVERIFY(!settings->contains(QStringLiteral("win/arrangements/bakeout/state")));
    QVERIFY(!settings->contains(QStringLiteral("win/arrangements/bakeout/geometry")));
  }

  void namesAreSortedIgnoringCase() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    for (const char* name : {"running", "Bakeout", "air"}) QVERIFY(win.layouts->save_as(QString::fromLatin1(name)));
    QCOMPARE(win.layouts->names(),
             QStringList({QStringLiteral("air"), QStringLiteral("Bakeout"), QStringLiteral("running")}));
  }

  void savingOverANameThatDiffersInCaseReplacesIt() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    QVERIFY(win.layouts->save_as(QStringLiteral("running")));
    win.a->close();
    QVERIFY(win.layouts->save_as(QStringLiteral("Running")));
    QCOMPARE(win.layouts->names(), QStringList{QStringLiteral("Running")});
    QVERIFY(win.layouts->contains(QStringLiteral("RUNNING")));
    win.layouts->reset();
    QVERIFY(win.layouts->apply(QStringLiteral("running")));
    QVERIFY(!win.a->isVisible());  // the second one's layout
  }

  void namesTheIniFormatManglesRoundTrip() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    const QStringList hostile{QString::fromUtf8("Ünï 100% [a]=b"), QStringLiteral("..."), QStringLiteral("a.b"),
                              QStringLiteral("General")};
    for (const QString& name : hostile) {
      QVERIFY2(win.layouts->save_as(name), qPrintable(name));
      QVERIFY2(win.layouts->names().contains(name), qPrintable(name));
    }
    QCOMPARE(win.layouts->names().size(), hostile.size());
    // as another run of the application reads the file
    const auto again = ini(dir);
    Window other(again.get());
    other.show();
    for (const QString& name : hostile) {
      QVERIFY2(other.layouts->names().contains(name), qPrintable(name));
      QVERIFY2(other.layouts->apply(name), qPrintable(name));
    }
  }

  void garbageArrangementGivesFactoryAndAnError() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    settings->setValue(QStringLiteral("win/arrangements/bad/state"), QByteArray("zz"));
    Window win(settings.get());
    win.show();
    win.a->close();
    const QSignalSpy spy(win.layouts, &DockLayouts::applyFailed);
    const auto applied = win.layouts->apply(QStringLiteral("bad"));
    QVERIFY(!applied);
    QCOMPARE(applied.error().kind, ErrorKind::Config);
    QCOMPARE(applied.error().code, std::string("bad_layout"));
    win.verify_factory();
    QCOMPARE(spy.count(), 1);
    QVERIFY2(spy[0][0].toString().startsWith(QString::fromUtf8("arrangement “bad” not applied: ")),
             qPrintable(spy[0][0].toString()));
  }

  void unknownArrangementLeavesTheLayoutAlone() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->close();
    const QSignalSpy spy(win.layouts, &DockLayouts::applyFailed);
    const auto applied = win.layouts->apply(QStringLiteral("nope"));
    QVERIFY(!applied);
    QCOMPARE(applied.error().code, std::string("unknown_name"));
    QVERIFY(!win.a->isVisible());
    QCOMPARE(spy.count(), 0);
  }

  void aMissingGeometryStillAppliesTheState() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->close();
    QVERIFY(win.layouts->save_as(QStringLiteral("x")));
    settings->remove(QStringLiteral("win/arrangements/x/geometry"));
    win.layouts->reset();
    QVERIFY(win.layouts->apply(QStringLiteral("x")));
    QVERIFY(!win.a->isVisible());
  }

  void aDockAddedSinceTheArrangementWasSavedStaysShown() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    {
      Window before(settings.get());
      before.show();
      before.a->close();
      QVERIFY(before.layouts->save_as(QStringLiteral("old")));
    }
    Window after(settings.get(), true);
    after.show();
    QVERIFY(after.layouts->apply(QStringLiteral("old")));
    QVERIFY(!after.a->isVisible());
    QVERIFY(after.d->isVisible());
    QVERIFY(!after.d->isFloating());
  }

  void withoutSettingsSaveAsFails() {
    Window win(nullptr);
    const auto saved = win.layouts->save_as(QStringLiteral("x"));
    QVERIFY(!saved);
    QCOMPARE(saved.error().code, std::string("no_settings"));
    QVERIFY(win.layouts->names().isEmpty());
    QVERIFY(!win.layouts->contains(QStringLiteral("x")));
    const auto applied = win.layouts->apply(QStringLiteral("x"));
    QVERIFY(!applied);
    QCOMPARE(applied.error().code, std::string("unknown_name"));
  }

  void aBadNameIsNotSaved() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    const auto saved = win.layouts->save_as(QStringLiteral("a/b"));
    QVERIFY(!saved);
    QCOMPARE(saved.error().code, std::string("bad_name"));
    QVERIFY(win.layouts->names().isEmpty());
  }

  void anUnwritableSettingsFileIsReported() {
    QTemporaryDir dir;
    QFile blocker(dir.filePath(QStringLiteral("f")));  // a file where the settings' directory would be
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    QSettings settings(dir.filePath(QStringLiteral("f/s.ini")), QSettings::IniFormat);
    Window win(&settings);
    const auto saved = win.layouts->save_as(QStringLiteral("x"));
    QVERIFY(!saved);
    QCOMPARE(saved.error().kind, ErrorKind::Io);
    QCOMPARE(saved.error().code, std::string("not_saved"));
    QVERIFY(win.layouts->names().isEmpty());
  }

  void applyingToAMaximizedWindowShowsThePanels() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->close();
    QVERIFY(win.layouts->save_as(QStringLiteral("small")));
    win.layouts->reset();
    win.w.showMaximized();
    QTRY_VERIFY(win.w.isMaximized());
    QVERIFY(win.layouts->apply(QStringLiteral("small")));
    QVERIFY(!win.a->isVisible());
    QTRY_VERIFY(win.b->isVisible());
    QVERIFY(QGuiApplication::primaryScreen()->availableGeometry().intersects(win.w.frameGeometry()));
  }
};

QTEST_MAIN(TestDockLayouts)
#include "test_dock_layouts.moc"
