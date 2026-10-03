// The shortcut catalog and Help > Keyboard Shortcuts: no two keys clash where
// both are live, the menus and widgets take their keys from the catalog (so
// the reference is what the keys do), and the dialog lists and filters it.

#include <QtTest/QtTest>

#include <set>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "script_editor_window.hpp"
#include "shortcuts.hpp"
#include "ui_fixture.hpp"

using pychron::ui::MenuHub;
using pychron::ui::Shortcut;
using pychron::ui::ShortcutContext;
using pychron::ui::ShortcutsDialog;
using pychron::ui::shortcut_catalog;

namespace {

QList<QAction*> menu_actions(QMainWindow& w) {
  QList<QAction*> out;
  for (QMenu* m : MenuHub::instance().menus(qobject_cast<QMenuBar*>(w.menuWidget())))
    for (QAction* a : m->actions())
      if (!a->isSeparator()) out << a;
  return out;
}

// The commands the dialog shows, by group.
QStringList shown(const ShortcutsDialog& d) {
  QStringList out;
  for (int g = 0; g < d.tree()->topLevelItemCount(); ++g) {
    const QTreeWidgetItem* group = d.tree()->topLevelItem(g);
    if (group->isHidden()) continue;
    for (int i = 0; i < group->childCount(); ++i)
      if (!group->child(i)->isHidden()) out << group->child(i)->text(0);
  }
  return out;
}

}  // namespace

class TestShortcuts : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;

 private slots:
  void every_shortcut_has_one_entry() {
    std::set<Shortcut> ids;
    for (const auto& e : shortcut_catalog()) {
      QVERIFY2(ids.insert(e.id).second, qPrintable(e.command));
      QVERIFY(!e.command.isEmpty());
    }
    QCOMPARE(pychron::ui::key(Shortcut::SaveQueue), QKeySequence(QKeySequence::Save));
    QCOMPARE(pychron::ui::key(Shortcut::StartQueue), QKeySequence(Qt::Key_F5));
  }

  // Window keys may repeat between windows (Save is Save queue in one and
  // Save script in another), but never within one, nor against Everywhere.
  void no_two_live_keys_clash() {
    const auto& c = shortcut_catalog();
    for (std::size_t i = 0; i < c.size(); ++i) {
      for (std::size_t j = i + 1; j < c.size(); ++j) {
        if (c[i].key.isEmpty() || c[i].key != c[j].key) continue;
        const bool overlap = c[i].context == c[j].context || c[i].context == ShortcutContext::Everywhere ||
                             c[j].context == ShortcutContext::Everywhere;
        QVERIFY2(!overlap, qPrintable(QStringLiteral("%1 and %2 share %3")
                                          .arg(c[i].command, c[j].command, c[i].key.toString())));
      }
    }
  }

  // Keys are spelled out in shortcuts.cpp and nowhere else.
  void no_widget_source_spells_out_a_key() {
    const QRegularExpression literal(QStringLiteral("QKeySequence\\((Qt::|QKeySequence::[A-Z])|QKeySequence::[A-Z][a-z]+\\b(?!Text)"));
    const QDir dir(QStringLiteral(PYCHRON_UI_SOURCE_DIR));
    QStringList found;
    for (const QString& name : dir.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.hpp")}, QDir::Files)) {
      if (name.startsWith(QStringLiteral("shortcuts."))) continue;
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
      int number = 0;
      while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        ++number;
        if (literal.match(line).hasMatch()) found << QStringLiteral("%1:%2: %3").arg(name).arg(number).arg(line.trimmed());
      }
    }
    QVERIFY2(found.isEmpty(), qPrintable(found.join(QLatin1Char('\n'))));
  }

  // Every key in the menus is in the catalog, and every catalog key that
  // belongs to a menu is in one.
  void the_menus_use_the_catalog() {
    pychron::ui::test::SimLab sim;
    pychron::ui::ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::MainWindow main(*sim.line);
    pychron::ui::ExperimentWindow experiment(
        bridge, true, std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    pychron::ui::ScriptEditorWindow* editor = experiment.open_script_editor();
    main.show();
    experiment.show();

    std::set<QString> in_menus;
    for (QAction* a : menu_actions(main) + menu_actions(experiment) + menu_actions(*editor)) {
      if (a->shortcut().isEmpty()) continue;
      in_menus.insert(a->shortcut().toString());
      const bool known = std::any_of(shortcut_catalog().begin(), shortcut_catalog().end(),
                                     [&](const auto& e) { return e.key == a->shortcut(); });
      QVERIFY2(known, qPrintable(a->text() + QStringLiteral(" has ") + a->shortcut().toString()));
    }
    for (const auto& e : shortcut_catalog()) {
      if (e.key.isEmpty() || e.context == ShortcutContext::DataBrowser || e.id == Shortcut::AddRuns) continue;
      QVERIFY2(in_menus.count(e.key.toString()) == 1, qPrintable(e.command + QStringLiteral(" is in no menu")));
    }
  }

  void the_dialog_lists_and_filters_the_catalog() {
    ShortcutsDialog dialog;
    std::size_t bound = 0;
    for (const auto& e : shortcut_catalog()) bound += e.key.isEmpty() ? 0 : 1;
    QCOMPARE(static_cast<std::size_t>(shown(dialog).size()), bound);
    QCOMPARE(dialog.tree()->topLevelItem(0)->text(0), QStringLiteral("Everywhere"));

    dialog.filter()->setText(QStringLiteral("queue"));
    QVERIFY(shown(dialog).contains(QStringLiteral("Save queue")));
    QVERIFY(shown(dialog).contains(QStringLiteral("Start the queue")));
    QVERIFY(!shown(dialog).contains(QStringLiteral("Save script")));
    QVERIFY(dialog.tree()->topLevelItem(0)->isHidden());  // nothing Everywhere matches

    dialog.filter()->setText(QKeySequence(Qt::Key_F5).toString(QKeySequence::NativeText));  // by key
    QCOMPARE(shown(dialog), QStringList{QStringLiteral("Start the queue")});
    dialog.filter()->clear();
    QCOMPARE(static_cast<std::size_t>(shown(dialog).size()), bound);
  }

  void help_opens_the_reference_from_any_window() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    QMainWindow figure;
    figure.show();
    QList<QAction*> help;
    for (QMenu* m : MenuHub::instance().menus(qobject_cast<QMenuBar*>(figure.menuWidget())))
      if (m->title().remove(QLatin1Char('&')) == QStringLiteral("Help")) help = m->actions();
    QCOMPARE(help.size(), 4);  // Command Palette…, Keyboard Shortcuts, separator, About
    QCOMPARE(help.at(1)->text(), QStringLiteral("Keyboard Shortcuts"));
    QCOMPARE(help.last(), main.about_action());
    QCOMPARE(help.at(1)->shortcut(), pychron::ui::key(Shortcut::KeyboardShortcuts));

    help.at(1)->trigger();
    auto* dialog = main.findChild<ShortcutsDialog*>();
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    dialog->filter()->setText(QStringLiteral("zzz"));
    dialog->close();
    help.at(1)->trigger();  // the same dialog again, unfiltered
    QCOMPARE(main.findChildren<ShortcutsDialog*>().size(), 1);
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->filter()->text().isEmpty());
  }
};

QTEST_MAIN(TestShortcuts)
#include "test_shortcuts.moc"
