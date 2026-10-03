// The command palette: it offers what can run now (not disabled, hidden or
// itself), finds commands by a few letters, runs one with Enter, and opened
// over a window keeps that window's own commands live.

#include <QtTest/QtTest>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QTreeWidget>

#include "command_palette.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "shortcuts.hpp"
#include "ui_fixture.hpp"

using pychron::ui::CommandPalette;
using pychron::ui::fuzzy_score;
using pychron::ui::MenuHub;

namespace {

QAction* palette_action(pychron::ui::MainWindow& main) {
  for (QMenu* m : MenuHub::instance().menus(qobject_cast<QMenuBar*>(main.menuWidget())))
    for (QAction* a : m->actions())
      if (a->text() == QStringLiteral("Command Palette…")) return a;
  return nullptr;
}

CommandPalette* open(pychron::ui::MainWindow& main) {
  palette_action(main)->trigger();
  auto* palette = main.findChild<CommandPalette*>();
  return palette != nullptr && palette->isVisible() ? palette : nullptr;
}

}  // namespace

class TestCommandPalette : public QObject {
  Q_OBJECT

 private slots:
  void init() { QCoreApplication::processEvents(); }

  void fuzzy_matching() {
    QCOMPARE(fuzzy_score(QStringLiteral("Queue › Save"), QString()), 0);
    QVERIFY(fuzzy_score(QStringLiteral("Queue › Save"), QStringLiteral("xyz")) < 0);
    QVERIFY(fuzzy_score(QStringLiteral("Queue › Save"), QStringLiteral("QS")) >= 0);  // any case
    QVERIFY(fuzzy_score(QStringLiteral("Queue › Save"), QStringLiteral("q s")) >= 0);  // spaces ignored
    // Word starts beat letters buried in a word.
    QVERIFY(fuzzy_score(QStringLiteral("Window › Extraction Line"), QStringLiteral("exl")) >
            fuzzy_score(QStringLiteral("Executor › Cancel"), QStringLiteral("exl")));
    // Typed as written beats scattered.
    QVERIFY(fuzzy_score(QStringLiteral("Queue › Save"), QStringLiteral("save")) >
            fuzzy_score(QStringLiteral("Scripts › Script Editor"), QStringLiteral("save")));
    // Preferring a later word start never loses a match the rest needs.
    QVERIFY(fuzzy_score(QStringLiteral("xab ac"), QStringLiteral("ab")) >= 0);
  }

  void labels_name_the_menu_without_mnemonics_or_ellipsis() {
    QAction a(QStringLiteral("Save &As..."));
    QCOMPARE(CommandPalette::label(&a, QStringLiteral("&Queue")), QStringLiteral("Queue › Save As"));
    QAction b(QStringLiteral("Preferences…"));
    QCOMPARE(CommandPalette::label(&b, QStringLiteral("&File")), QStringLiteral("File › Preferences"));
  }

  void it_offers_what_can_run_now() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    main.show();
    QCOMPARE(palette_action(main)->shortcut(), pychron::ui::key(pychron::ui::Shortcut::CommandPalette));
    CommandPalette* palette = open(main);
    QVERIFY(palette != nullptr);
    const QStringList shown = palette->shown();
    QVERIFY(shown.contains(QStringLiteral("Window › Extraction Line")));
    QVERIFY(shown.contains(QStringLiteral("File › Preferences")));
    QVERIFY(shown.contains(QStringLiteral("Help › About pychron")));
    QVERIFY(!shown.contains(QStringLiteral("Window › Spectrometer")));      // disabled: none loaded
    QVERIFY(!shown.contains(QStringLiteral("File › Installations")));       // hidden
    QVERIFY(!shown.contains(QStringLiteral("Help › Command Palette")));     // not itself
    QVERIFY(palette->filter()->hasFocus() || !QApplication::focusWidget());

    palette->filter()->setText(QStringLiteral("exl"));
    QCOMPARE(palette->shown().first(), QStringLiteral("Window › Extraction Line"));
    palette->filter()->setText(QStringLiteral("qqqq"));
    QVERIFY(palette->shown().isEmpty());
  }

  void enter_runs_the_highlighted_command_and_escape_runs_none() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    main.show();
    CommandPalette* palette = open(main);
    QVERIFY(palette != nullptr);
    palette->filter()->setText(QStringLiteral("keyboard"));
    QCOMPARE(palette->shown().first(), QStringLiteral("Help › Keyboard Shortcuts"));
    QTest::keyClick(palette->filter(), Qt::Key_Escape);
    QVERIFY(!palette->isVisible());
    QCoreApplication::processEvents();
    QVERIFY(main.findChild<pychron::ui::ShortcutsDialog*>() == nullptr);

    palette = open(main);
    QVERIFY(palette != nullptr);
    QVERIFY(palette->filter()->text().isEmpty());  // a fresh start each time
    palette->filter()->setText(QStringLiteral("keyboard"));
    QTest::keyClick(palette->filter(), Qt::Key_Return);
    QVERIFY(!palette->isVisible());
    QTRY_VERIFY(main.findChild<pychron::ui::ShortcutsDialog*>() != nullptr);
  }

  void the_arrow_keys_move_the_highlight() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    main.show();
    CommandPalette* palette = open(main);
    QVERIFY(palette != nullptr);
    QTreeWidget* list = palette->list();
    QVERIFY(list->topLevelItemCount() > 2);
    QCOMPARE(list->indexOfTopLevelItem(list->currentItem()), 0);
    QTest::keyClick(palette->filter(), Qt::Key_Down);
    QTest::keyClick(palette->filter(), Qt::Key_Down);
    QCOMPARE(list->indexOfTopLevelItem(list->currentItem()), 2);
    QTest::keyClick(palette->filter(), Qt::Key_Up);
    QCOMPARE(list->indexOfTopLevelItem(list->currentItem()), 1);
    QTest::keyClick(palette->filter(), Qt::Key_Up);
    QTest::keyClick(palette->filter(), Qt::Key_Up);  // stops at the top
    QCOMPARE(list->indexOfTopLevelItem(list->currentItem()), 0);
    QTest::keyClick(palette->filter(), Qt::Key_Escape);
  }

  // Opened over a window, the palette offers that window's own commands and
  // they still run: the palette does not take the window's place as active.
  void a_windows_own_commands_stay_live_under_the_palette() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    QMainWindow experiment;
    auto* revalidate = new QAction(QStringLiteral("&Revalidate"), &experiment);
    int ran = 0;
    connect(revalidate, &QAction::triggered, this, [&] { ++ran; });
    MenuHub::instance().contribute(&experiment, MenuHub::Menu::Queue, {revalidate}, MenuHub::Scope::Window);
    main.show();
    experiment.show();

    // Over a window without it, it is not offered.
    main.activateWindow();
    if (!QTest::qWaitForWindowActive(&main)) QSKIP("this platform does not activate windows");
    QTRY_COMPARE(QApplication::activeWindow(), &main);
    CommandPalette* palette = open(main);
    QVERIFY(palette != nullptr);
    QVERIFY(!palette->shown().contains(QStringLiteral("Queue \u203a Revalidate")));
    QTest::keyClick(palette->filter(), Qt::Key_Escape);

    experiment.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&experiment));
    QTRY_COMPARE(QApplication::activeWindow(), &experiment);
    palette = open(main);
    QVERIFY(palette != nullptr);
    // What a window manager does with a new window; a popup does not take it.
    palette->activateWindow();
    QTest::qWait(50);
    QCOMPARE(QApplication::activeWindow(), &experiment);
    QVERIFY(revalidate->isEnabled());
    QVERIFY(palette->shown().contains(QStringLiteral("Queue \u203a Revalidate")));
    palette->filter()->setText(QStringLiteral("reval"));
    QTest::keyClick(palette->filter(), Qt::Key_Return);
    QTRY_COMPARE(ran, 1);
  }
};

QTEST_MAIN(TestCommandPalette)
#include "test_command_palette.moc"
