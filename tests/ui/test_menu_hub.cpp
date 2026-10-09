// The unified menu bar: every window shows the same menus in the same order,
// app actions work from any window, a window's own actions only while it is
// active (so one shortcut can mean different things in different windows),
// and empty menus are hidden everywhere.

#include <QtTest/QtTest>

#include <functional>
#include <memory>
#include <optional>

#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QDialog>
#include <QMainWindow>
#include <QStatusBar>
#include <QSettings>
#include <QLabel>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "dock_layouts.hpp"
#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "shortcuts.hpp"
#include "settings_guard.hpp"
#include "ui_fixture.hpp"

using pychron::ui::MenuHub;
using Menu = MenuHub::Menu;
using Scope = MenuHub::Scope;

namespace {

QMenuBar* bar_of(QWidget& w) { return MenuHub::instance().bar_for(&w); }

// Counts `action` being taken out of the menus it is in.
struct RemovalWatch : QObject {
  QAction* action;
  int removed = 0;
  explicit RemovalWatch(QAction* a) : action(a) {}
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::ActionRemoved && static_cast<QActionEvent*>(e)->action() == action) ++removed;
    return false;
  }
};

// The titles of the menus `w` shows, without mnemonics.
QStringList shown(QWidget& w) {
  QStringList out;
  QMenuBar* bar = bar_of(w);
  if (bar == nullptr) return out;
  for (QMenu* m : MenuHub::instance().menus(bar))
    if (m->menuAction()->isVisible()) out << m->title().remove(QLatin1Char('&'));
  return out;
}

QStringList texts(QMenu* menu) {
  QStringList out;
  for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
  return out;
}

QMenu* menu_of(QWidget& w, Menu menu) { return MenuHub::instance().menus(bar_of(w)).at(static_cast<int>(menu)); }

bool activate(QWidget& w) {
  w.activateWindow();
  return QTest::qWaitForWindowActive(&w);
}

// A main window with a closable dock per title and the helper that keeps
// their layout, in `settings` (null: it keeps nothing).
struct DockedWindow {
  DockedWindow(const QStringList& titles, QSettings* settings) {
    w.setCentralWidget(new QLabel(QStringLiteral("center")));
    for (const QString& title : titles) {
      auto* dock = new QDockWidget(title, &w);
      dock->setObjectName(title);
      dock->setWidget(new QLabel(title));
      docks.append(dock);
    }
    const std::function<void()> factory = [this] {
      for (QDockWidget* dock : docks) {
        dock->setFloating(false);
        w.addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->show();
      }
    };
    factory();
    layouts = new pychron::ui::DockLayouts(&w, factory, settings, QStringLiteral("win"));
    w.setWindowTitle(titles.join(QLatin1Char(' ')));
    w.resize(600, 400);
  }
  Q_DISABLE_COPY_MOVE(DockedWindow)
  ~DockedWindow() = default;

  QMainWindow w;
  QList<QDockWidget*> docks;
  pychron::ui::DockLayouts* layouts = nullptr;
};

QStringList texts_of(const QList<QAction*>& actions) {
  QStringList out;
  for (const QAction* a : actions) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
  return out;
}

QAction* named(QMenu* menu, const QString& text) {
  for (QAction* a : menu->actions())
    if (a->text() == text) return a;
  return nullptr;
}

// A plain window with a layout, like the recall and data browser windows.
struct PlainWindow : QWidget {
  PlainWindow() { new QVBoxLayout(this); }
};

}  // namespace

class TestMenuHub : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;
  pychron::ui::test::ApplicationSettingsGuard app_settings_;

 private slots:
  void init() { QCoreApplication::processEvents(); }  // the last test's windows leave the menus
  void cleanup() {
    MenuHub::instance().set_arrangement_asks({});  // the dialogs again
    QCoreApplication::processEvents();
    if (MenuHub::instance().bars() != MenuHub::platform_bars()) MenuHub::reset(MenuHub::platform_bars());
  }

  void every_window_shows_the_same_menus() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    QMainWindow figure;  // a secondary window that contributes nothing
    PlainWindow recall;
    QDialog dialog;
    new QVBoxLayout(&dialog);
    for (QWidget* w : {static_cast<QWidget*>(&main), static_cast<QWidget*>(&figure), static_cast<QWidget*>(&recall),
                       static_cast<QWidget*>(&dialog)})
      w->show();

    // The whole bar, from launch: the experiment's menus are there before
    // the experiment window is.
    const QStringList expected{QStringLiteral("File"), QStringLiteral("Queue"), QStringLiteral("Rows"),
                               QStringLiteral("Executor"), QStringLiteral("Scripts"), QStringLiteral("View"),
                               QStringLiteral("Window"), QStringLiteral("Help")};
    QCOMPARE(shown(main), expected);
    QCOMPARE(shown(figure), expected);
    QCOMPARE(shown(recall), expected);
    QVERIFY(bar_of(dialog) == nullptr);  // dialogs keep no bar
    // One bar on macOS, a copy per window elsewhere.
    QCOMPARE(bar_of(figure) == bar_of(main), MenuHub::platform_bars() == MenuHub::Bars::Shared);

    // The same actions, not copies: File > Preferences in the figure window
    // is the main window's.
    QVERIFY(menu_of(figure, Menu::File)->actions().contains(main.preferences_action()));
    QVERIFY(menu_of(recall, Menu::View)->actions().contains(main.spectrometer_action()));
    QCOMPARE(texts(menu_of(figure, Menu::View)).first(), QStringLiteral("Extraction Line"));
    QVERIFY(menu_of(figure, Menu::Help)->actions().contains(main.about_action()));

    // Every View item wears a glyph, shown in the menu even where the
    // platform hides menu icons (macOS), and no two are the same drawing.
    QList<QImage> glyphs;
    for (const QAction* action : menu_of(figure, Menu::View)->actions()) {
      if (action->isSeparator()) continue;
      QVERIFY2(!action->icon().isNull(), qPrintable(action->text()));
      QVERIFY2(action->isIconVisibleInMenu(), qPrintable(action->text()));
      QVERIFY(action->icon().isMask());
      const QImage image = action->icon().pixmap(18, 18).toImage();
      QVERIFY(!glyphs.contains(image));
      glyphs.append(image);
    }
    QCOMPARE(glyphs.size(), 5);  // extraction line, spectrometer, experiment, data, laser
  }

  void fit_menu_sits_between_entry_and_window() {
    QMainWindow main;
    main.show();
    // The titles as the bar shows them, left to right.
    const auto titles = [&main] {
      QStringList out;
      for (const QAction* a : bar_of(main)->actions())
        if (a->isVisible()) out << a->text().remove(QLatin1Char('&'));
      return out;
    };
    // Nothing contributed: neither menu is shown.
    QVERIFY(!titles().contains(QStringLiteral("Fit")));
    QVERIFY(!titles().contains(QStringLiteral("Entry")));
    QVERIFY(!menu_of(main, Menu::Fit)->menuAction()->isVisible());
    QVERIFY(MenuHub::instance().placeholder(Menu::Fit) == nullptr);
    QCOMPARE(MenuHub::title(Menu::Fit), QStringLiteral("F&it"));

    QAction samples(QStringLiteral("Samples…"));
    QAction flux(QStringLiteral("Flux…"));
    MenuHub::instance().contribute(&main, Menu::Fit, {&flux}, Scope::App);
    QStringList shown_now = titles();
    QVERIFY(!shown_now.contains(QStringLiteral("Entry")));  // Fit alone
    QCOMPARE(shown_now.indexOf(QStringLiteral("Window")), shown_now.indexOf(QStringLiteral("Fit")) + 1);
    QVERIFY(shown_now.indexOf(QStringLiteral("Fit")) > shown_now.indexOf(QStringLiteral("Scripts")));

    MenuHub::instance().contribute(&main, Menu::Entry, {&samples}, Scope::App);
    shown_now = titles();
    const qsizetype entry = shown_now.indexOf(QStringLiteral("Entry"));
    QVERIFY(entry >= 0);
    QCOMPARE(shown_now.indexOf(QStringLiteral("Scripts")), entry - 1);  // View has nothing without a main window
    QCOMPARE(shown_now.indexOf(QStringLiteral("Fit")), entry + 1);
    QCOMPARE(shown_now.indexOf(QStringLiteral("Window")), entry + 2);
    QCOMPARE(shown_now.size(), entry + 3);  // nor Help
    // Among all ten, hidden ones included: ..., View, Entry, Fit, Window, Help.
    QStringList all;
    for (const QAction* a : bar_of(main)->actions()) all << a->text().remove(QLatin1Char('&'));
    QCOMPARE(all.mid(5), (QStringList{QStringLiteral("View"), QStringLiteral("Entry"), QStringLiteral("Fit"),
                                      QStringLiteral("Window"), QStringLiteral("Help")}));
    QCOMPARE(texts(menu_of(main, Menu::Fit)), QStringList{QStringLiteral("Flux…")});
    // The existing slots keep their values.
    QCOMPARE(static_cast<int>(Menu::Entry), 8);
    QCOMPARE(static_cast<int>(Menu::Fit), 9);
    QCOMPARE(MenuHub::kMenus, std::size_t{10});
  }

  void the_shared_bar_has_fit_between_entry_and_window() {
    MenuHub& hub = MenuHub::reset(MenuHub::Bars::Shared);
    QMainWindow main;
    main.show();
    QMenuBar* bar = hub.bar_for(&main);
    QVERIFY(bar != nullptr);
    QVERIFY(bar->parentWidget() == nullptr);  // the one bar
    const auto titles = [bar](bool hidden) {
      QStringList out;
      for (const QAction* a : bar->actions())
        if (hidden || a->isVisible()) out << a->text().remove(QLatin1Char('&'));
      return out;
    };
    QVERIFY(!titles(false).contains(QStringLiteral("Fit")));  // nothing contributed
    QVERIFY(!titles(false).contains(QStringLiteral("Entry")));

    QAction samples(QStringLiteral("Samples…"));
    QAction flux(QStringLiteral("Flux…"));
    hub.contribute(&main, Menu::Entry, {&samples}, Scope::App);
    hub.contribute(&main, Menu::Fit, {&flux}, Scope::App);
    const QStringList now = titles(false);
    const qsizetype entry = now.indexOf(QStringLiteral("Entry"));
    QVERIFY(entry >= 0);
    QCOMPARE(now.indexOf(QStringLiteral("Fit")), entry + 1);
    QCOMPARE(now.indexOf(QStringLiteral("Window")), entry + 2);
    QCOMPARE(titles(true).mid(5), (QStringList{QStringLiteral("View"), QStringLiteral("Entry"), QStringLiteral("Fit"),
                                               QStringLiteral("Window"), QStringLiteral("Help")}));
    QCOMPARE(texts(menu_of(main, Menu::Fit)), QStringList{QStringLiteral("Flux…")});
    // Every title's mnemonic is its own.
    QStringList mnemonics;
    for (const QAction* a : bar->actions()) {
      const qsizetype at = a->text().indexOf(QLatin1Char('&'));
      QVERIFY2(at >= 0, qPrintable(a->text()));
      const QString letter = a->text().mid(at + 1, 1).toLower();
      QVERIFY2(!mnemonics.contains(letter), qPrintable(a->text()));
      mnemonics << letter;
    }
    QCOMPARE(mnemonics.size(), 10);
  }

  void shared_one_bar_serves_every_window() {
    MenuHub& hub = MenuHub::reset(MenuHub::Bars::Shared);
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    QMainWindow figure;
    PlainWindow recall;
    QDialog dialog;
    new QVBoxLayout(&dialog);
    for (QWidget* w : {static_cast<QWidget*>(&main), static_cast<QWidget*>(&figure), static_cast<QWidget*>(&recall),
                       static_cast<QWidget*>(&dialog)})
      w->show();

    QMenuBar* bar = hub.bar_for(&main);
    QVERIFY(bar != nullptr);
    QVERIFY(bar->parentWidget() == nullptr);  // Qt's global bar on macOS
    QCOMPARE(hub.bar_for(&figure), bar);
    QCOMPARE(hub.bar_for(&recall), bar);
    QVERIFY(hub.bar_for(&dialog) == nullptr);
    // No window has a bar of its own.
    QVERIFY(main.menuWidget() == nullptr);
    QVERIFY(figure.menuWidget() == nullptr);
    QVERIFY(recall.layout()->menuBar() == nullptr);
    QCOMPARE(shown(main), (QStringList{QStringLiteral("File"), QStringLiteral("Queue"), QStringLiteral("Rows"),
                               QStringLiteral("Executor"), QStringLiteral("Scripts"), QStringLiteral("View"),
                               QStringLiteral("Window"), QStringLiteral("Help")}));
    QVERIFY(menu_of(main, Menu::File)->actions().contains(main.preferences_action()));
    QVERIFY(menu_of(main, Menu::Help)->actions().contains(main.about_action()));

    // Another window's actions come and go in the same bar.
    auto* owner = new QMainWindow;
    auto* save = new QAction(QStringLiteral("Save"), owner);
    hub.contribute(owner, Menu::Queue, {save}, Scope::Window);
    QCOMPARE(texts(menu_of(figure, Menu::Queue)), QStringList{QStringLiteral("Save")});
    delete owner;
    QTRY_COMPARE(menu_of(figure, Menu::Queue)->actions(), QList<QAction*>{hub.placeholder(Menu::Queue)});
  }

  // On macOS taking Preferences, Quit or About out of a menu hides its item in
  // the application menu, so a change elsewhere must leave them in place.
  void actions_that_stay_are_never_taken_out() {
    for (const auto bars : {MenuHub::Bars::PerWindow, MenuHub::Bars::Shared}) {
      MenuHub& hub = MenuHub::reset(bars);
      auto line = pychron::ui::test::make_example_line();
      pychron::ui::MainWindow main(*line);
      QMainWindow figure;
      main.show();
      figure.show();
      RemovalWatch watch(main.preferences_action());
      for (QMenu* m : hub.menus(hub.bar_for(&figure))) m->installEventFilter(&watch);
      for (QMenu* m : hub.menus(hub.bar_for(&main))) m->installEventFilter(&watch);

      auto* owner = new QMainWindow;
      auto* first = new QAction(QStringLiteral("first"), owner);
      auto* last = new QAction(QStringLiteral("last"), owner);
      hub.contribute(owner, Menu::File, {first}, Scope::App);  // after Preferences' group
      hub.contribute(owner, Menu::Queue, {last}, Scope::Window);
      QCOMPARE(texts(menu_of(figure, Menu::File)).last(), QStringLiteral("first"));
      delete owner;
      // the deferred rebuild has run: Queue is back to its greyed line
      QTRY_COMPARE(menu_of(figure, Menu::Queue)->actions(), QList<QAction*>{hub.placeholder(Menu::Queue)});
      QVERIFY(!texts(menu_of(figure, Menu::File)).contains(QStringLiteral("first")));

      QCOMPARE(watch.removed, 0);
      QVERIFY(menu_of(figure, Menu::File)->actions().contains(main.preferences_action()));
      QVERIFY(menu_of(main, Menu::File)->actions().contains(main.preferences_action()));
      QVERIFY(!menu_of(figure, Menu::File)->actions().last()->isSeparator());  // no stray separator
      QCoreApplication::processEvents();
      if (bars != MenuHub::platform_bars()) MenuHub::reset(MenuHub::platform_bars());
    }
  }

  // Window is the usual one: Minimize, Zoom, Bring All to Front, then every
  // open window by title. Dialogs are not windows to switch to.
  void the_window_menu_lists_the_open_windows() {
    MenuHub& hub = MenuHub::instance();
    QMainWindow a;
    a.setWindowTitle(QStringLiteral("Extraction Line"));
    PlainWindow b;
    b.setWindowTitle(QStringLiteral("Recall"));
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Preferences"));
    a.show();
    b.show();
    dialog.show();
    auto titles = [&] {
      QStringList out;
      for (const QAction* action : hub.window_actions()) out.append(action->text());
      return out;
    };
    const QStringList both{QStringLiteral("Extraction Line"), QStringLiteral("Recall")};
    QTRY_COMPARE(titles(), both);
    // in the menu of every window, after the three fixed entries
    for (QWidget* w : {static_cast<QWidget*>(&a), static_cast<QWidget*>(&b)}) {
      QCOMPARE(texts(menu_of(*w, Menu::Window)),
               (QStringList{QStringLiteral("Minimize"), QStringLiteral("Zoom"), QStringLiteral("|"),
                            QStringLiteral("Bring All to Front"), QStringLiteral("|"), QStringLiteral("Panels"),
                            QStringLiteral("Arrangements"), QStringLiteral("Reset Layout"), QStringLiteral("|"),
                            QStringLiteral("Extraction Line"), QStringLiteral("Recall")}));
    }
    QVERIFY(shown(a).contains(QStringLiteral("Window")));

    // a new title shows; a closed window leaves; a minimized one stays
    b.setWindowTitle(QStringLiteral("Recall 12345"));
    QTRY_COMPARE(titles(), (QStringList{QStringLiteral("Extraction Line"), QStringLiteral("Recall 12345")}));
    b.showMinimized();
    QCoreApplication::processEvents();
    QCOMPARE(titles().size(), 2);
    b.close();
    QTRY_COMPARE(titles(), QStringList{QStringLiteral("Extraction Line")});

    // choosing a window brings it back and forward
    a.showMinimized();
    QTRY_VERIFY(a.isMinimized());
    hub.window_actions().first()->trigger();
    QTRY_VERIFY(!a.isMinimized());

    QCOMPARE(hub.minimize_action()->shortcut(), pychron::ui::key(pychron::ui::Shortcut::MinimizeWindow));
  }

  // ---- Window > Panels, Arrangements, Reset Layout: the window in front's ------

  void the_window_menu_has_the_layout_commands() {
    for (const MenuHub::Bars bars : {MenuHub::Bars::PerWindow, MenuHub::Bars::Shared}) {
      MenuHub& hub = MenuHub::reset(bars);
      {
        QMainWindow w;
        w.setWindowTitle(QStringLiteral("One"));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QList<QAction*> all = menu_of(w, Menu::Window)->actions();
        QTRY_COMPARE(texts_of(menu_of(w, Menu::Window)->actions()),
                     (QStringList{QStringLiteral("Minimize"), QStringLiteral("Zoom"), QStringLiteral("|"),
                                  QStringLiteral("Bring All to Front"), QStringLiteral("|"), QStringLiteral("Panels"),
                                  QStringLiteral("Arrangements"), QStringLiteral("Reset Layout"), QStringLiteral("|"),
                                  QStringLiteral("One")}));
        QVERIFY(menu_of(w, Menu::Window)->actions().contains(hub.panels_action()));
        QVERIFY(menu_of(w, Menu::Window)->actions().contains(hub.arrangements_action()));
        QVERIFY(menu_of(w, Menu::Window)->actions().contains(hub.reset_layout_action()));
        QVERIFY(hub.panels_action()->menu() != nullptr);
        QVERIFY(hub.arrangements_action()->menu() != nullptr);
      }
      QCoreApplication::processEvents();
    }
  }

  void layout_commands_follow_the_window_in_front() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("follow.ini")), QSettings::IniFormat);
    QMainWindow plain;
    plain.show();
    QVERIFY(activate(plain));
    QTRY_VERIFY(!hub.panels_action()->isEnabled());
    QVERIFY(!hub.arrangements_action()->isEnabled());
    QVERIFY(!hub.reset_layout_action()->isEnabled());

    DockedWindow one({QStringLiteral("A"), QStringLiteral("B")}, &settings);
    one.w.show();
    QVERIFY(activate(one.w));
    QTRY_VERIFY(hub.panels_action()->isEnabled());
    QVERIFY(hub.arrangements_action()->isEnabled());
    QVERIFY(hub.reset_layout_action()->isEnabled());
    emit hub.panels_action()->menu()->aboutToShow();
    QCOMPARE(texts(hub.panels_action()->menu()), (QStringList{QStringLiteral("A"), QStringLiteral("B")}));

    DockedWindow two({QStringLiteral("X")}, &settings);
    two.w.show();
    QVERIFY(activate(two.w));
    QTRY_COMPARE(hub.current_window(), &two.w);
    emit hub.panels_action()->menu()->aboutToShow();
    QCOMPARE(texts(hub.panels_action()->menu()), QStringList{QStringLiteral("X")});

    one.docks[0]->close();
    two.docks[0]->close();
    hub.reset_layout_action()->trigger();
    QVERIFY(two.docks[0]->isVisible());
    QVERIFY(!one.docks[0]->isVisible());  // the other window's stays as it was
  }

  void save_arrangement_asks_again_after_a_bad_name() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("badname.ini")), QSettings::IniFormat);
    DockedWindow win({QStringLiteral("A")}, &settings);
    win.w.show();
    QVERIFY(activate(win.w));
    QTRY_VERIFY(hub.save_arrangement_action()->isEnabled());
    QStringList answers{QStringLiteral("a/b"), QStringLiteral("bakeout")};
    QStringList refused;
    hub.set_arrangement_asks({.name = [&](QWidget*) -> std::optional<QString> {
                                if (answers.isEmpty()) return std::nullopt;
                                return answers.takeFirst();
                              },
                              .replace = [](QWidget*, const QString&) { return true; },
                              .refuse = [&](QWidget*, const QString& why) { refused.append(why); }});
    hub.save_arrangement_action()->trigger();
    QCOMPARE(refused.size(), 1);
    QVERIFY(!refused.first().isEmpty());
    QCOMPARE(win.layouts->names(), QStringList{QStringLiteral("bakeout")});
    QVERIFY(answers.isEmpty());
  }

  void save_arrangement_asks_before_replacing() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("replace.ini")), QSettings::IniFormat);
    DockedWindow win({QStringLiteral("A")}, &settings);
    win.w.show();
    QVERIFY(activate(win.w));
    QVERIFY(win.layouts->save_as(QStringLiteral("bakeout")));
    int asked = 0;
    bool replace = false;
    QStringList asked_about;
    std::optional<QString> second;
    hub.set_arrangement_asks({.name = [&](QWidget*) -> std::optional<QString> {
                                return ++asked == 1 ? std::optional<QString>(QStringLiteral("Bakeout")) : second;
                              },
                              .replace = [&](QWidget*, const QString& name) {
                                asked_about.append(name);
                                return replace;
                              },
                              .refuse = [](QWidget*, const QString&) {}});
    hub.save_arrangement_action()->trigger();  // refused, then cancelled
    QCOMPARE(asked, 2);
    QCOMPARE(asked_about, QStringList{QStringLiteral("bakeout")});  // the one that is there
    QCOMPARE(win.layouts->names(), QStringList{QStringLiteral("bakeout")});

    asked = 0;
    replace = true;
    hub.save_arrangement_action()->trigger();
    QCOMPARE(asked, 1);
    QCOMPARE(win.layouts->names(), QStringList{QStringLiteral("Bakeout")});
  }

  void save_arrangement_is_disabled_without_settings() {
    MenuHub& hub = MenuHub::instance();
    DockedWindow win({QStringLiteral("A")}, nullptr);
    win.w.show();
    QVERIFY(activate(win.w));
    QTRY_VERIFY(hub.reset_layout_action()->isEnabled());
    QVERIFY(!hub.save_arrangement_action()->isEnabled());
    bool asked = false;
    hub.set_arrangement_asks({.name = [&](QWidget*) -> std::optional<QString> {
                                asked = true;
                                return std::nullopt;
                              },
                              .replace = [](QWidget*, const QString&) { return true; },
                              .refuse = [](QWidget*, const QString&) {}});
    hub.save_arrangement_action()->trigger();
    QVERIFY(!asked);
  }

  void the_arrangements_menu_applies_and_deletes() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("menu.ini")), QSettings::IniFormat);
    DockedWindow win({QStringLiteral("A"), QStringLiteral("B")}, &settings);
    win.w.show();
    QVERIFY(activate(win.w));
    QTRY_VERIFY(hub.arrangements_action()->isEnabled());
    QMenu* menu = hub.arrangements_action()->menu();

    emit menu->aboutToShow();  // none yet
    QCOMPARE(texts(menu), (QStringList{QString::fromUtf8("Save Arrangement As…"), QStringLiteral("Delete")}));
    QVERIFY(!named(menu, QStringLiteral("Delete"))->isVisible());

    win.docks[0]->close();
    QVERIFY(win.layouts->save_as(QStringLiteral("bakeout")));
    win.layouts->reset();
    QVERIFY(win.layouts->save_as(QStringLiteral("running")));
    emit menu->aboutToShow();
    QCOMPARE(texts(menu), (QStringList{QStringLiteral("bakeout"), QStringLiteral("running"), QStringLiteral("|"),
                                       QString::fromUtf8("Save Arrangement As…"), QStringLiteral("Delete")}));
    QVERIFY(menu->actions().contains(hub.save_arrangement_action()));
    QAction* remove = named(menu, QStringLiteral("Delete"));
    QVERIFY(remove->isVisible());
    QVERIFY(remove->menu() != nullptr);
    QCOMPARE(texts(remove->menu()), (QStringList{QStringLiteral("bakeout"), QStringLiteral("running")}));

    named(menu, QStringLiteral("bakeout"))->trigger();
    QVERIFY(!win.docks[0]->isVisible());

    emit menu->aboutToShow();
    named(named(menu, QStringLiteral("Delete"))->menu(), QStringLiteral("running"))->trigger();
    QCOMPARE(win.layouts->names(), QStringList{QStringLiteral("bakeout")});
  }

  void a_name_with_an_ampersand_is_shown_and_applied_as_typed() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("amp.ini")), QSettings::IniFormat);
    DockedWindow win({QStringLiteral("A")}, &settings);
    win.w.show();
    QVERIFY(activate(win.w));
    win.docks[0]->close();
    QVERIFY(win.layouts->save_as(QStringLiteral("air & blank")));
    win.layouts->reset();
    QMenu* menu = hub.arrangements_action()->menu();
    emit menu->aboutToShow();
    QAction* item = menu->actions().first();
    QCOMPARE(item->text(), QStringLiteral("air && blank"));  // a literal ampersand in a menu
    item->trigger();
    QVERIFY(!win.docks[0]->isVisible());
  }

  void a_failed_apply_is_shown_in_the_status_bar() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("failed.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("win/arrangements/bad/state"), QByteArray("zz"));
    DockedWindow win({QStringLiteral("A")}, &settings);
    win.w.show();
    QVERIFY(activate(win.w));
    QMenu* menu = hub.arrangements_action()->menu();
    emit menu->aboutToShow();
    QVERIFY(named(menu, QStringLiteral("bad")) != nullptr);
    named(menu, QStringLiteral("bad"))->trigger();
    QVERIFY2(win.w.statusBar()->currentMessage().startsWith(QString::fromUtf8("arrangement “bad” not applied: ")),
             qPrintable(win.w.statusBar()->currentMessage()));
  }

  void the_command_palette_has_reset_and_save() {
    MenuHub& hub = MenuHub::instance();
    bool reset = false;
    bool save = false;
    for (const MenuHub::Command& c : hub.commands()) {
      if (c.action == hub.reset_layout_action() && c.menu == Menu::Window) reset = true;
      if (c.action == hub.save_arrangement_action() && c.menu == Menu::Window) save = true;
    }
    QVERIFY(reset);
    QVERIFY(save);
  }

  void layout_commands_without_a_front_window_do_nothing() {
    MenuHub& hub = MenuHub::instance();
    QSettings settings(tmp_.filePath(QStringLiteral("front.ini")), QSettings::IniFormat);
    DockedWindow win({QStringLiteral("A")}, &settings);
    win.w.show();
    QVERIFY(activate(win.w));
    QTRY_VERIFY(hub.reset_layout_action()->isEnabled());

    // a popup over the window (the command palette is one): the window under it
    win.docks[0]->close();
    {
      QWidget popup(&win.w, Qt::Popup);
      popup.resize(50, 50);
      popup.show();
      QCoreApplication::processEvents();
      hub.reset_layout_action()->trigger();
      QVERIFY(win.docks[0]->isVisible());
    }

    // a dialog in front: not a window with panels
    win.docks[0]->close();
    {
      QDialog dialog(&win.w);
      dialog.show();
      QVERIFY(activate(dialog));
      QTRY_VERIFY(!hub.reset_layout_action()->isEnabled());
      QVERIFY(!hub.panels_action()->isEnabled());
      hub.reset_layout_action()->trigger();
      hub.save_arrangement_action()->trigger();
      emit hub.panels_action()->menu()->aboutToShow();
      emit hub.arrangements_action()->menu()->aboutToShow();
      QVERIFY(hub.panels_action()->menu()->isEmpty());
      QVERIFY(!win.docks[0]->isVisible());
    }

    // no window at all
    win.w.hide();
    QTRY_VERIFY(!hub.reset_layout_action()->isEnabled());
    hub.reset_layout_action()->trigger();
    hub.save_arrangement_action()->trigger();
    QVERIFY(win.docks[0]->isHidden());
  }

  // The experiment's menus keep their place in the bar: one greyed line
  // until a window fills them, and again when it has gone.
  void an_experiment_menu_holds_a_greyed_line_until_a_window_fills_it() {
    MenuHub& hub = MenuHub::instance();
    QMainWindow a;
    a.show();
    for (const Menu menu : {Menu::Queue, Menu::Rows, Menu::Executor, Menu::Scripts}) {
      QVERIFY(hub.placeholder(menu) != nullptr);
      QVERIFY(!hub.placeholder(menu)->isEnabled());
      QVERIFY(menu_of(a, menu)->menuAction()->isVisible());
      QCOMPARE(menu_of(a, menu)->actions(), QList<QAction*>{hub.placeholder(menu)});
    }
    QVERIFY(hub.placeholder(Menu::File) == nullptr);  // only those four
    QVERIFY(shown(a).contains(QStringLiteral("Queue")));

    auto* owner = new QMainWindow;
    auto* save = new QAction(QStringLiteral("Save"), owner);
    hub.contribute(owner, Menu::Queue, {save}, Scope::Window);
    // at once, in a window that was already up; the greyed line makes way
    QCOMPARE(texts(menu_of(a, Menu::Queue)), QStringList{QStringLiteral("Save")});
    delete owner;
    QTRY_COMPARE(menu_of(a, Menu::Queue)->actions(), QList<QAction*>{hub.placeholder(Menu::Queue)});
    QVERIFY(shown(a).contains(QStringLiteral("Queue")));
    // the greyed line is no command for the palette
    for (const MenuHub::Command& c : hub.commands()) QVERIFY(c.action != hub.placeholder(c.menu));
  }

  void groups_are_separated_in_contribution_order() {
    QMainWindow a;
    a.show();
    QMainWindow owner;
    auto* x = new QAction(QStringLiteral("x"), &owner);
    auto* y = new QAction(QStringLiteral("y"), &owner);
    auto* z = new QAction(QStringLiteral("z"), &owner);
    MenuHub::instance().contribute(&owner, Menu::Scripts, {x, y}, Scope::App);
    MenuHub::instance().contribute(&owner, Menu::Scripts, {z}, Scope::App);
    QCOMPARE(texts(menu_of(a, Menu::Scripts)),
             (QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("|"), QStringLiteral("z")}));
  }

  void window_actions_follow_the_active_window() {
    QMainWindow a;
    QMainWindow b;
    auto* save_a = new QAction(QStringLiteral("Save queue"), &a);
    auto* save_b = new QAction(QStringLiteral("Save script"), &b);
    auto* app = new QAction(QStringLiteral("Everywhere"), &a);
    save_a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    save_b->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));  // the same key, in the same bar
    app->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J));
    int saved_a = 0, saved_b = 0, everywhere = 0;
    connect(save_a, &QAction::triggered, this, [&] { ++saved_a; });
    connect(save_b, &QAction::triggered, this, [&] { ++saved_b; });
    connect(app, &QAction::triggered, this, [&] { ++everywhere; });
    MenuHub::instance().contribute(&a, Menu::Queue, {save_a}, Scope::Window);
    MenuHub::instance().contribute(&b, Menu::Scripts, {save_b}, Scope::Window);
    MenuHub::instance().contribute(&a, Menu::View, {app}, Scope::App);
    a.show();
    b.show();

    if (!activate(a)) QSKIP("this platform does not activate windows");
    QVERIFY(save_a->isEnabled());
    QVERIFY(!save_b->isEnabled());
    QTest::keyClick(&a, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(saved_a, 1);
    QCOMPARE(saved_b, 0);

    QVERIFY(activate(b));
    QVERIFY(!save_a->isEnabled());
    QVERIFY(save_b->isEnabled());
    QTest::keyClick(&b, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(saved_a, 1);
    QCOMPARE(saved_b, 1);
    // The menu still shows both, with their shortcuts.
    QCOMPARE(save_a->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_S));

    // An app action works from a window that is not its owner's.
    QTest::keyClick(&b, Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(everywhere, 1);
  }

  void a_window_still_decides_its_own_actions_enabled_state() {
    QMainWindow a;
    QMainWindow other;
    auto* save = new QAction(QStringLiteral("Save"), &a);
    MenuHub::instance().contribute(&a, Menu::Queue, {save}, Scope::Window);
    a.show();
    other.show();
    if (!activate(a)) QSKIP("this platform does not activate windows");
    save->setEnabled(false);  // nothing to save
    QVERIFY(activate(other));
    QVERIFY(activate(a));
    QVERIFY(!save->isEnabled());  // becoming active does not override it
    save->setEnabled(true);
    QVERIFY(save->isEnabled());
    QVERIFY(activate(other));
    save->setEnabled(true);  // and inactive wins over the window's own say-so
    QVERIFY(!save->isEnabled());
  }

  void the_experiment_window_fills_the_queue_rows_executor_and_scripts_menus() {
    pychron::ui::test::SimLab sim;
    pychron::ui::ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::ExperimentWindow window(
        bridge, true, std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    window.show();
    // Window is the hub's own and always there.
    QCOMPARE(shown(window), (QStringList{QStringLiteral("Queue"), QStringLiteral("Rows"), QStringLiteral("Executor"),
                                         QStringLiteral("Scripts"), QStringLiteral("Window")}));
    QVERIFY(texts(menu_of(window, Menu::Queue)).contains(QStringLiteral("&Save")));
    QVERIFY(texts(menu_of(window, Menu::Executor)).contains(QStringLiteral("Start")));
    QVERIFY(texts(menu_of(window, Menu::Scripts)).contains(QStringLiteral("Script &Editor...")));
    // And a window with nothing of its own shows them too.
    QMainWindow figure;
    figure.show();
    QCOMPARE(shown(figure), shown(window));
  }
  // Every window here was given its settings: none fell back to the application's.
  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(TestMenuHub)
#include "test_menu_hub.moc"
