#include "command_palette.hpp"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "menu_hub.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

constexpr int kVisibleRows = 10;
constexpr int kWidth = 560;
const char* const kPaletteProperty = "pychron_command_palette";

bool word_start(const QString& t, qsizetype i) { return i == 0 || !t.at(i - 1).isLetterOrNumber(); }

// Whether q[k...] is a subsequence of t[from...].
bool rest_matches(const QString& t, const QString& q, qsizetype k, qsizetype from) {
  for (; k < q.size(); ++k) {
    from = t.indexOf(q.at(k), from);
    if (from < 0) return false;
    ++from;
  }
  return true;
}

}  // namespace

int fuzzy_score(const QString& text, const QString& query) {
  QString q;
  for (const QChar c : query)
    if (!c.isSpace()) q += c.toLower();
  if (q.isEmpty()) return 0;
  const QString t = text.toLower();
  int score = 1000;
  qsizetype pos = 0;
  qsizetype prev = -2;
  for (qsizetype k = 0; k < q.size(); ++k) {
    const QChar c = q.at(k);
    qsizetype i = t.indexOf(c, pos);
    if (i < 0) return -1;
    // Prefer this letter where a word starts further on, if the rest of the
    // query still fits after it ("sv" takes the S of Save, not of Queue's).
    if (!word_start(t, i)) {
      for (qsizetype j = t.indexOf(c, i + 1); j >= 0; j = t.indexOf(c, j + 1)) {
        if (word_start(t, j) && rest_matches(t, q, k + 1, j + 1)) {
          i = j;
          break;
        }
      }
    }
    if (word_start(t, i)) score += 10;
    if (i == prev + 1) score += 6;
    score -= static_cast<int>(std::min<qsizetype>(i - pos, 4));  // letters skipped
    prev = i;
    pos = i + 1;
  }
  if (t.contains(q)) score += 20;  // typed as it is written
  return score;
}

QString CommandPalette::label(const QAction* action, const QString& menu_title) {
  QString text = action->text();
  text.remove(QLatin1Char('&'));
  while (text.endsWith(QLatin1Char('.')) || text.endsWith(QChar(0x2026))) text.chop(1);
  QString menu = menu_title;
  menu.remove(QLatin1Char('&'));
  return menu + QStringLiteral(" › ") + text.trimmed();
}

CommandPalette::CommandPalette(QWidget* parent)
    // A popup, and one that never takes the active window's place: the window's
    // own commands stay live under it (Qt refuses the activation a window
    // manager, or a platform without one, would otherwise hand a new window).
    : QFrame(parent, Qt::Popup | Qt::WindowDoesNotAcceptFocus), filter_(new QLineEdit), list_(new QTreeWidget) {
  setObjectName(QStringLiteral("CommandPalette"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  filter_->setPlaceholderText(tr("Type a command"));
  QFont big = filter_->font();
  big.setPointSizeF(big.pointSizeF() * 1.15);
  filter_->setFont(big);
  filter_->installEventFilter(this);
  layout->addWidget(filter_);

  list_->setColumnCount(2);
  list_->setHeaderHidden(true);
  list_->setRootIsDecorated(false);
  list_->setUniformRowHeights(true);
  list_->setFocusPolicy(Qt::NoFocus);  // typing stays in the filter
  list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  list_->header()->setStretchLastSection(false);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  layout->addWidget(list_);

  connect(filter_, &QLineEdit::textChanged, this, [this] { refill(); });
  connect(list_, &QTreeWidget::itemClicked, this, [this] { run_current(); });
}

void CommandPalette::collect() {
  entries_.clear();
  for (const MenuHub::Command& c : MenuHub::instance().commands()) {
    QAction* a = c.action;
    if (a->isSeparator() || a->menu() != nullptr || !a->isVisible() || !a->isEnabled()) continue;
    if (a->property(kPaletteProperty).toBool()) continue;  // not itself
    if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.action == a; })) continue;
    entries_.append({a, label(a, MenuHub::title(c.menu))});
  }
}

void CommandPalette::refill() {
  const QString query = filter_->text();
  struct Ranked {
    int score;
    qsizetype index;
  };
  std::vector<Ranked> ranked;
  for (qsizetype i = 0; i < entries_.size(); ++i) {
    if (entries_[i].action == nullptr) continue;
    const int score = fuzzy_score(entries_[i].label, query);
    if (score >= 0) ranked.push_back({score, i});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });

  list_->clear();
  const QFont keys_font = style::mono_font();
  const int row = fontMetrics().height() + 12;
  for (const Ranked& r : ranked) {
    const Entry& e = entries_[r.index];
    auto* item = new QTreeWidgetItem(list_, {e.label, e.action->shortcut().toString(QKeySequence::NativeText)});
    item->setData(0, Qt::UserRole, static_cast<qlonglong>(r.index));
    item->setFont(1, keys_font);
    item->setForeground(1, theme().muted_text);
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    item->setSizeHint(0, QSize(0, row));
  }
  if (list_->topLevelItemCount() > 0) list_->setCurrentItem(list_->topLevelItem(0));
  list_->setFixedHeight(row * std::clamp(list_->topLevelItemCount(), 1, kVisibleRows) + 4);
  adjustSize();
}

QStringList CommandPalette::shown() const {
  QStringList out;
  for (int i = 0; i < list_->topLevelItemCount(); ++i) out << list_->topLevelItem(i)->text(0);
  return out;
}

void CommandPalette::open_over(QWidget* over) {
  collect();
  filter_->clear();
  refill();
  const int width = std::min(kWidth, std::max(320, over->width() - 40));
  setFixedWidth(width);
  const QPoint top = over->mapToGlobal(QPoint(over->width() / 2 - width / 2, std::min(80, over->height() / 6)));
  move(top);
  show();
  raise();
  filter_->setFocus();
}

void CommandPalette::run_current() {
  const QTreeWidgetItem* item = list_->currentItem();
  if (item == nullptr) return;
  const QPointer<QAction> action = entries_.value(item->data(0, Qt::UserRole).toLongLong()).action;
  hide();
  // Once the popup has gone and focus is back in the window behind it.
  QTimer::singleShot(0, this, [action] {
    if (action != nullptr && action->isEnabled()) action->trigger();
  });
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
  if (watched != filter_ || event->type() != QEvent::KeyPress) return QFrame::eventFilter(watched, event);
  const auto* key = static_cast<QKeyEvent*>(event);
  const int count = list_->topLevelItemCount();
  const int current = list_->currentItem() != nullptr ? list_->indexOfTopLevelItem(list_->currentItem()) : -1;
  const auto go = [&](int row) {
    if (count == 0) return;
    list_->setCurrentItem(list_->topLevelItem(std::clamp(row, 0, count - 1)));
  };
  switch (key->key()) {
    case Qt::Key_Down:
      go(current + 1);
      return true;
    case Qt::Key_Up:
      go(current - 1);
      return true;
    case Qt::Key_PageDown:
      go(current + kVisibleRows);
      return true;
    case Qt::Key_PageUp:
      go(current - kVisibleRows);
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      run_current();
      return true;
    case Qt::Key_Escape:
      hide();
      return true;
    default:
      return QFrame::eventFilter(watched, event);
  }
}

QAction* make_command_palette_action(QWidget* window) {
  auto* action = new QAction(QStringLiteral("Command Palette…"), window);
  action->setShortcut(key(Shortcut::CommandPalette));
  action->setProperty(kPaletteProperty, true);
  QObject::connect(action, &QAction::triggered, window, [window] {
    auto* palette = window->findChild<CommandPalette*>(QString(), Qt::FindDirectChildrenOnly);
    if (palette == nullptr) palette = new CommandPalette(window);
    // Over the window in front, whose own commands it then offers.
    QWidget* over = QApplication::activeWindow();
    if (over == nullptr || qobject_cast<QDialog*>(over) != nullptr || over == palette) over = window->window();
    palette->open_over(over);
  });
  return action;
}

}  // namespace pychron::ui
