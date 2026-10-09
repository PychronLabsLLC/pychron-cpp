#include "shortcuts.hpp"

#include <algorithm>

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSize>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "theme.hpp"

namespace pychron::ui {

namespace {

QKeySequence keys(QKeyCombination k) { return QKeySequence(k); }

}  // namespace

const std::vector<ShortcutEntry>& shortcut_catalog() {
  using C = ShortcutContext;
  using S = Shortcut;
  static const std::vector<ShortcutEntry> catalog{
      {S::Preferences, C::Everywhere, QStringLiteral("Preferences…"), QKeySequence(QKeySequence::Preferences)},
      {S::Quit, C::Everywhere, QStringLiteral("Quit"), QKeySequence(QKeySequence::Quit)},
      {S::ExtractionLineWindow, C::Everywhere, QStringLiteral("Extraction Line window"),
       keys(Qt::CTRL | Qt::SHIFT | Qt::Key_L)},
      {S::SpectrometerWindow, C::Everywhere, QStringLiteral("Spectrometer window"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_S)},
      {S::ExperimentWindow, C::Everywhere, QStringLiteral("Experiment window"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
      {S::DataWindow, C::Everywhere, QStringLiteral("Data browser"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
      {S::LaserWindow, C::Everywhere, QStringLiteral("Laser window"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
      {S::KeyboardShortcuts, C::Everywhere, QStringLiteral("Keyboard shortcuts (this list)"),
       QKeySequence(QKeySequence::HelpContents)},
      {S::CommandPalette, C::Everywhere, QStringLiteral("Command palette"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_P)},
      {S::MinimizeWindow, C::Everywhere, QStringLiteral("Minimize the window in front"), keys(Qt::CTRL | Qt::Key_M)},

      {S::FileNew, C::FileMenu, QStringLiteral("New…"), QKeySequence(QKeySequence::New)},
      {S::FileOpen, C::FileMenu, QStringLiteral("Open…"), QKeySequence(QKeySequence::Open)},
      {S::FileSave, C::FileMenu, QStringLiteral("Save"), QKeySequence(QKeySequence::Save)},

      {S::MoveRowsUp, C::ExperimentWindow, QStringLiteral("Move rows up"), keys(Qt::CTRL | Qt::Key_Up)},
      {S::MoveRowsDown, C::ExperimentWindow, QStringLiteral("Move rows down"), keys(Qt::CTRL | Qt::Key_Down)},
      {S::DuplicateRows, C::ExperimentWindow, QStringLiteral("Duplicate rows"), keys(Qt::CTRL | Qt::Key_D)},
      {S::DeleteRows, C::ExperimentWindow, QStringLiteral("Delete rows"), QKeySequence(QKeySequence::Delete)},
      {S::ToggleSkip, C::ExperimentWindow, QStringLiteral("Toggle skip"), keys(Qt::CTRL | Qt::Key_K)},
      {S::EndAfter, C::ExperimentWindow, QStringLiteral("End after this run"), keys(Qt::CTRL | Qt::Key_E)},
      {S::AddRuns, C::ExperimentWindow, QStringLiteral("Add runs (run factory)"), keys(Qt::CTRL | Qt::Key_Return)},
      {S::StartQueue, C::ExperimentWindow, QStringLiteral("Start the queue"), keys(Qt::Key_F5)},
      {S::ScriptEditor, C::ExperimentWindow, QStringLiteral("Script editor"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_K)},

      {S::CloseScriptTab, C::ScriptEditor, QStringLiteral("Close tab"), QKeySequence(QKeySequence::Close)},
      {S::CheckScript, C::ScriptEditor, QStringLiteral("Check now"), keys(Qt::Key_F7)},
      {S::GoToGosub, C::ScriptEditor, QStringLiteral("Go to the gosub under the cursor"), keys(Qt::Key_F2)},

      {S::RecallNext, C::DataBrowser, QStringLiteral("Recall the next analysis"), keys(Qt::CTRL | Qt::Key_N)},
      {S::RecallPrevious, C::DataBrowser, QStringLiteral("Recall the previous analysis"), keys(Qt::CTRL | Qt::Key_B)},
  };
  return catalog;
}

QKeySequence key(Shortcut id) {
  const auto& c = shortcut_catalog();
  const auto it = std::find_if(c.begin(), c.end(), [&](const ShortcutEntry& e) { return e.id == id; });
  return it == c.end() ? QKeySequence() : it->key;
}

bool overlap(ShortcutContext a, ShortcutContext b) {
  using C = ShortcutContext;
  if (a == b || a == C::Everywhere || b == C::Everywhere) return true;
  const auto answers_file = [](C c) { return c == C::ExperimentWindow || c == C::ScriptEditor; };
  return (a == C::FileMenu && answers_file(b)) || (b == C::FileMenu && answers_file(a));
}

QString context_name(ShortcutContext context) {
  switch (context) {
    case ShortcutContext::Everywhere:
      return QStringLiteral("Everywhere");
    case ShortcutContext::FileMenu:
      return QStringLiteral("Experiment window and editors");
    case ShortcutContext::ExperimentWindow:
      return QStringLiteral("Experiment window");
    case ShortcutContext::ScriptEditor:
      return QStringLiteral("Script editor");
    case ShortcutContext::DataBrowser:
      break;
  }
  return QStringLiteral("Data browser");
}

ShortcutsDialog::ShortcutsDialog(QWidget* parent)
    : QDialog(parent), filter_(new QLineEdit), tree_(new QTreeWidget) {
  setWindowTitle(tr("Keyboard Shortcuts"));
  auto* layout = new QVBoxLayout(this);
  filter_->setPlaceholderText(tr("Filter by command or key"));
  filter_->setClearButtonEnabled(true);
  layout->addWidget(filter_);

  tree_->setColumnCount(2);
  tree_->setHeaderLabels({tr("Command"), tr("Shortcut")});
  tree_->setRootIsDecorated(false);
  tree_->setSelectionMode(QAbstractItemView::NoSelection);
  tree_->setFocusPolicy(Qt::NoFocus);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree_->header()->setStretchLastSection(false);
  layout->addWidget(tree_, 1);

  QFont heading = font();
  heading.setBold(true);
  const QFont keys_font = style::mono_font();
  const int line = fontMetrics().height();
  for (const ShortcutContext context : {ShortcutContext::Everywhere, ShortcutContext::FileMenu,
                                        ShortcutContext::ExperimentWindow, ShortcutContext::ScriptEditor,
                                        ShortcutContext::DataBrowser}) {
    auto* group = new QTreeWidgetItem(tree_, {context_name(context)});
    group->setFirstColumnSpanned(true);
    group->setFont(0, heading);
    group->setForeground(0, theme().accent);
    group->setTextAlignment(0, Qt::AlignLeft | Qt::AlignBottom);  // space above each heading
    group->setSizeHint(0, QSize(0, line * 2 + 4));
    for (const ShortcutEntry& e : shortcut_catalog()) {
      if (e.context != context || e.key.isEmpty()) continue;  // no binding on this platform
      auto* item = new QTreeWidgetItem(group, {e.command, e.key.toString(QKeySequence::NativeText)});
      item->setFont(1, keys_font);
      item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
      item->setSizeHint(0, QSize(0, line + 10));
    }
  }
  tree_->expandAll();

  auto* note = new QLabel(tr("Window shortcuts work while that window is in front; the others work in every window."));
  note->setWordWrap(true);
  style::set_tone(note, style::Tone::Muted);
  layout->addWidget(note);

  connect(filter_, &QLineEdit::textChanged, this, &ShortcutsDialog::apply_filter);
  resize(480, 720);
}

void ShortcutsDialog::apply_filter(const QString& text) {
  const QString needle = text.trimmed();
  for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = tree_->topLevelItem(g);
    bool any = false;
    for (int i = 0; i < group->childCount(); ++i) {
      QTreeWidgetItem* item = group->child(i);
      const bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive) ||
                         item->text(1).contains(needle, Qt::CaseInsensitive);
      item->setHidden(!match);
      any = any || match;
    }
    group->setHidden(!any);
  }
}

QAction* make_shortcuts_action(QWidget* window) {
  auto* action = new QAction(QStringLiteral("Keyboard Shortcuts"), window);
  action->setShortcut(key(Shortcut::KeyboardShortcuts));
  QObject::connect(action, &QAction::triggered, window, [window] {
    auto* dialog = window->findChild<ShortcutsDialog*>(QString(), Qt::FindDirectChildrenOnly);
    if (dialog == nullptr) dialog = new ShortcutsDialog(window);
    dialog->filter()->clear();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
  });
  return action;
}

}  // namespace pychron::ui
