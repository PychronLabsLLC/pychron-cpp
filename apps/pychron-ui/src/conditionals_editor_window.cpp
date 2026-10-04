#include "conditionals_editor_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include "pychron/experiment/conditionals/validate.hpp"
#include "pychron/experiment/model/identifiers.hpp"
#include "theme.hpp"

namespace pychron::ui {

using experiment::ConditionalKind;
using experiment::ConditionalSet;

namespace {

const QString kSettingsGroup = QStringLiteral("conditionals_editor");

// What a conditional's analysis_types may name: the lab's run types, and
// "blank" for every blank type.
QStringList analysis_type_names() {
  using A = experiment::AnalysisType;
  QStringList out;
  for (const A a : {A::Unknown, A::Air, A::Cocktail}) out.append(QString::fromUtf8(to_string(a)));
  out.append(QStringLiteral("blank"));
  for (const A a : {A::BlankUnknown, A::BlankAir, A::BlankCocktail, A::BlankExtractionLine, A::Background,
                    A::DetectorIC, A::Degas})
    out.append(QString::fromUtf8(to_string(a)));
  return out;
}

// A '#' outside a string: a comment the canonical file would not have.
bool has_comment(const std::string& text) {
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (quote != 0) {
      if (ch == '\\' && quote == '"') ++i;
      else if (ch == quote || ch == '\n') quote = 0;
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '#') {
      return true;
    }
  }
  return false;
}

QToolButton* tool(const QString& text, const QString& tip) {
  auto* b = new QToolButton;
  b->setText(text);
  b->setToolTip(tip);
  return b;
}

}  // namespace

ConditionalsEditorWindow::ConditionalsEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings,
                                                   QWidget* parent)
    : QMainWindow(parent),
      lab_(lab),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      files_(new QListWidget),
      table_(new QTableView),
      form_(new ConditionalForm(analysis_type_names())),
      disable_(new QListWidget),
      diagnostics_(new QListWidget),
      status_(new QLabel) {
  resize(980, 720);
  files_->setObjectName(QStringLiteral("files"));
  disable_->setObjectName(QStringLiteral("disable"));
  diagnostics_->setObjectName(QStringLiteral("diagnostics"));

  table_->setModel(&model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->horizontalHeader()->setSectionResizeMode(ConditionalTableModel::Check, QHeaderView::Stretch);
  table_->horizontalHeader()->setStretchLastSection(false);

  // Files.
  auto* new_button = tool(QStringLiteral("+"), tr("New conditionals file"));
  auto* delete_button = tool(QStringLiteral("-"), tr("Delete the selected file"));
  auto* file_buttons = new QHBoxLayout;
  file_buttons->addWidget(new_button);
  file_buttons->addWidget(delete_button);
  file_buttons->addStretch(1);
  auto* left = new QWidget;
  auto* left_col = new QVBoxLayout(left);
  left_col->addWidget(new QLabel(tr("Files")));
  left_col->addWidget(files_, 1);
  left_col->addLayout(file_buttons);

  // Rows.
  auto* add_button = tool(tr("Add"), tr("Add a conditional"));
  auto* add_menu = new QMenu(add_button);
  for (const ConditionalKind k : experiment::kFileOrder)
    add_menu->addAction(QString::fromUtf8(to_string(k)), this, [this, k] { add_conditional(k); });
  add_button->setMenu(add_menu);
  add_button->setPopupMode(QToolButton::InstantPopup);
  auto* remove_button = tool(tr("Remove"), tr("Remove the selected conditional"));
  auto* duplicate_button = tool(tr("Duplicate"), tr("Copy the selected conditional"));
  auto* up_button = tool(tr("Up"), tr("Move up within its kind"));
  auto* down_button = tool(tr("Down"), tr("Move down within its kind"));
  auto* row_buttons = new QHBoxLayout;
  for (QWidget* b : {add_button, remove_button, duplicate_button, up_button, down_button}) row_buttons->addWidget(b);
  row_buttons->addStretch(1);

  auto* disable_add = tool(QStringLiteral("+"), tr("Disable a conditional of an earlier level by name"));
  auto* disable_remove = tool(QStringLiteral("-"), tr("Remove the selected name"));
  auto* disable_buttons = new QVBoxLayout;
  disable_buttons->addWidget(disable_add);
  disable_buttons->addWidget(disable_remove);
  disable_buttons->addStretch(1);
  auto* disable_row = new QHBoxLayout;
  disable_row->addWidget(disable_, 1);
  disable_row->addLayout(disable_buttons);
  disable_->setMaximumHeight(70);

  auto* right = new QWidget;
  auto* right_col = new QVBoxLayout(right);
  right_col->addLayout(row_buttons);
  right_col->addWidget(table_, 2);
  right_col->addWidget(form_);
  right_col->addWidget(new QLabel(tr("Disables (names from earlier levels)")));
  right_col->addLayout(disable_row);
  right_col->addWidget(new QLabel(tr("Diagnostics")));
  right_col->addWidget(diagnostics_, 1);

  auto* split = new QSplitter;
  split->addWidget(left);
  split->addWidget(right);
  split->setStretchFactor(1, 1);
  split->setSizes({180, 800});
  setCentralWidget(split);
  statusBar()->addWidget(status_, 1);

  edit_controls_ = {add_button, remove_button, duplicate_button, up_button, down_button,
                    disable_add, disable_remove, form_,          disable_};

  auto* save_action = new QAction(tr("Save"), this);
  save_action->setShortcut(QKeySequence::Save);
  addAction(save_action);
  connect(save_action, &QAction::triggered, this, [this] {
    QString error;
    if (!save(&error) && !error.isEmpty()) QMessageBox::warning(this, tr("Save"), error);
  });

  ask_unsaved_ = [this](const QString& name) {
    const auto b =
        QMessageBox::question(this, tr("Unsaved conditionals"), tr("%1 has unsaved changes. Save them?").arg(name),
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    return b == QMessageBox::Save ? Unsaved::Save : b == QMessageBox::Discard ? Unsaved::Discard : Unsaved::Cancel;
  };
  confirm_ = [this](const QString& question) {
    return QMessageBox::question(this, tr("Conditionals"), question) == QMessageBox::Yes;
  };

  check_timer_.setSingleShot(true);
  check_timer_.setInterval(kCheckDelayMs);
  connect(&check_timer_, &QTimer::timeout, this, [this] { check_now(); });

  connect(files_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
    const QString name = item->data(Qt::UserRole).toString();
    if (name != current_ && !open(name)) fill_files();  // put the highlight back
  });
  connect(files_, &QListWidget::itemClicked, files_, &QListWidget::itemActivated);
  connect(new_button, &QToolButton::clicked, this, [this] {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("New conditionals file"), tr("Name"), QLineEdit::Normal, QString(), &ok);
    QString error;
    if (ok && !new_file(name.trimmed(), &error) && !error.isEmpty()) QMessageBox::warning(this, tr("New file"), error);
  });
  connect(delete_button, &QToolButton::clicked, this, [this] {
    QString error;
    if (!current_.isEmpty() && !delete_file(current_, &error) && !error.isEmpty())
      QMessageBox::warning(this, tr("Delete"), error);
  });
  connect(remove_button, &QToolButton::clicked, this, [this] {
    const int row = current_row();
    if (model_.remove(row)) select_row(std::min(row, model_.rowCount() - 1));
  });
  connect(duplicate_button, &QToolButton::clicked, this, [this] { select_row(model_.duplicate(current_row())); });
  connect(up_button, &QToolButton::clicked, this, [this] {
    const int row = current_row();
    if (model_.move_up(row)) select_row(row - 1);
  });
  connect(down_button, &QToolButton::clicked, this, [this] {
    const int row = current_row();
    if (model_.move_down(row)) select_row(row + 1);
  });
  connect(disable_add, &QToolButton::clicked, this, [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Disable"), tr("Name of the conditional to disable"),
                                               QLineEdit::Normal, QString(), &ok);
    if (ok) add_disable(name.trimmed());
  });
  connect(disable_remove, &QToolButton::clicked, this, [this] {
    QStringList names = model_.disable();
    const int row = disable_->currentRow();
    if (row < 0 || row >= names.size()) return;
    names.removeAt(row);
    model_.set_disable(names);
    fill_disable();
  });

  connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { on_selection(); });
  connect(form_, &ConditionalForm::edited, this, [this](const experiment::Conditional& c) {
    const int row = current_row();
    if (row < 0 || !editable_) return;
    const int moved = model_.replace(row, c);
    if (moved != row) select_row(moved);  // the kind changed; the form already shows it
  });
  connect(&model_, &ConditionalTableModel::changed, this, [this] {
    update_title();
    show_diagnostics(false);
    check_timer_.start();
  });
  connect(diagnostics_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
    const int row = model_.row_of(item->data(Qt::UserRole).toString());
    if (row >= 0) select_row(row);
  });
  connect(diagnostics_, &QListWidget::itemClicked, diagnostics_, &QListWidget::itemActivated);

  settings_->beginGroup(kSettingsGroup);
  if (auto g = settings_->value(QStringLiteral("geometry")).toByteArray(); !g.isEmpty()) restoreGeometry(g);
  const QString last = settings_->value(QStringLiteral("last")).toString();
  settings_->endGroup();

  show_nothing();
  const QStringList names = file_names();
  if (names.contains(last)) load(last);
  else if (!names.isEmpty()) load(names.first());
}

ConditionalsEditorWindow::~ConditionalsEditorWindow() = default;

QStringList ConditionalsEditorWindow::file_names() const {
  QStringList out;
  if (auto names = lab_.condition_files->list())
    for (const auto& n : *names) out.append(QString::fromStdString(n));
  if (!new_name_.isEmpty() && !out.contains(new_name_)) out.append(new_name_);
  return out;
}

void ConditionalsEditorWindow::fill_files() {
  const QSignalBlocker block(files_);
  files_->clear();
  for (const QString& name : file_names()) {
    auto* item = new QListWidgetItem(name == new_name_ ? name + QStringLiteral(" *") : name, files_);
    item->setData(Qt::UserRole, name);
    if (name == current_) files_->setCurrentItem(item);
  }
}

void ConditionalsEditorWindow::fill_disable() {
  disable_->clear();
  disable_->addItems(model_.disable());
}

void ConditionalsEditorWindow::set_editable(bool on) {
  editable_ = on;
  for (QWidget* w : edit_controls_) w->setEnabled(on);
}

void ConditionalsEditorWindow::show_nothing() {
  check_timer_.stop();
  current_.clear();
  load_error_.clear();
  has_comments_ = false;
  model_.set({});
  form_->set_conditional({});
  set_editable(false);
  fill_disable();
  fill_files();
  diagnostics_->clear();
  status_->setText(tr("No conditionals file. + creates one."));
  update_title();
}

void ConditionalsEditorWindow::load(const QString& name) {
  check_timer_.stop();
  current_ = name;
  load_error_.clear();
  has_comments_ = false;
  ConditionalSet set;
  bool ok = true;
  if (name != new_name_) {
    auto text = lab_.condition_files->read(name.toStdString());
    auto parsed = text ? experiment::parse_conditionals(*text, name.toStdString() + ".toml")
                       : Result<ConditionalSet>(fail(text.error()));
    if (parsed) {
      set = std::move(*parsed);
      has_comments_ = has_comment(*text);
    } else {
      ok = false;
      load_error_ = QString::fromStdString(parsed.error().what);
    }
  }
  model_.set(std::move(set));
  set_editable(ok);
  fill_disable();
  fill_files();
  diagnostics_->clear();
  if (ok) {
    status_->setText(QString::fromStdString(lab_.condition_files->path(name.toStdString()).string()));
    show_diagnostics(true);
  } else {
    status_->setText(tr("%1 cannot be edited: %2").arg(name, load_error_));
    auto* item = new QListWidgetItem(tr("error: %1").arg(load_error_), diagnostics_);
    item->setForeground(theme().error_text);
  }
  if (model_.rowCount() > 0) select_row(0);
  else form_->set_conditional({});
  form_->setEnabled(ok && model_.rowCount() > 0);
  update_title();
}

bool ConditionalsEditorWindow::modified() const {
  return editable_ && (model_.modified() || (!current_.isEmpty() && current_ == new_name_));
}

bool ConditionalsEditorWindow::resolve_unsaved() {
  if (!modified()) return true;
  switch (ask_unsaved_(current_)) {
    case Unsaved::Cancel: return false;
    case Unsaved::Save: {
      QString error;
      if (save(&error)) return true;
      if (!error.isEmpty()) status_->setText(error);
      return false;
    }
    case Unsaved::Discard:
      new_name_.clear();  // a new file that was never saved is gone
      return true;
  }
  return false;
}

bool ConditionalsEditorWindow::open(const QString& name) {
  if (name == current_) return true;
  if (!file_names().contains(name) || name == new_name_) return false;
  if (!resolve_unsaved()) return false;
  load(name);
  return true;
}

bool ConditionalsEditorWindow::new_file(const QString& name, QString* error) {
  auto refuse = [&](const QString& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (!experiment::ConditionalFiles::valid_name(name.toStdString()))
    return refuse(tr("'%1' is not a plain file name (no folders, no leading dot, no .toml)").arg(name));
  if (file_names().contains(name)) return refuse(tr("%1 already exists").arg(name));
  if (!resolve_unsaved()) return refuse(QString());
  new_name_ = name;
  load(name);
  return true;
}

bool ConditionalsEditorWindow::delete_file(const QString& name, QString* error) {
  if (!file_names().contains(name)) {
    if (error != nullptr) *error = tr("no file named %1").arg(name);
    return false;
  }
  QString question = tr("Delete %1.toml?").arg(name);
  if (referenced_ && referenced_(name)) question += tr(" The open queue uses it.");
  if (!confirm_(question)) return false;
  if (name == new_name_) {
    new_name_.clear();
  } else if (auto r = lab_.condition_files->remove(name.toStdString()); !r) {
    if (error != nullptr) *error = QString::fromStdString(r.error().what);
    return false;
  }
  if (name == current_) {
    const QStringList names = file_names();
    if (names.isEmpty()) show_nothing();
    else load(names.first());
  } else {
    fill_files();
  }
  emit filesChanged();
  return true;
}

bool ConditionalsEditorWindow::save(QString* error) {
  auto refuse = [&](const QString& why) {
    if (error != nullptr) *error = why;
    if (!why.isEmpty()) status_->setText(why);
    return false;
  };
  if (current_.isEmpty()) return refuse(tr("No file is open"));
  if (!editable_) return refuse(tr("%1 cannot be edited: %2").arg(current_, load_error_));
  if (model_.has_errors()) {
    int n = 0;
    for (int row = 0; row < model_.rowCount(); ++row) n += model_.error(row).isEmpty() ? 0 : 1;
    return refuse(tr("Fix the errors before saving (%1 conditional(s))").arg(n));
  }
  if (has_comments_ &&
      !confirm_(tr("%1.toml has comments; saving rewrites the file and drops them. Save?").arg(current_)))
    return refuse(QString());
  const std::string text = experiment::to_toml(model_.conditionals());
  if (auto r = lab_.condition_files->write(current_.toStdString(), text); !r)
    return refuse(QString::fromStdString(r.error().what));
  const bool created = current_ == new_name_;
  new_name_.clear();
  has_comments_ = false;
  model_.mark_clean();
  update_title();
  if (created) fill_files();
  // The queue-wide sets are read when a queue starts; a run's own files per run.
  status_->setText(current_ == QStringLiteral("system")
                       ? tr("Saved %1. Applies from the next queue start.").arg(current_)
                       : tr("Saved %1. Applies from the next run (next queue start when used as queue conditionals).")
                             .arg(current_));
  emit saved(current_);
  if (created) emit filesChanged();
  return true;
}

int ConditionalsEditorWindow::add_conditional(ConditionalKind kind) {
  if (!editable_) return -1;
  const int row = model_.add(kind);
  select_row(row);
  return row;
}

void ConditionalsEditorWindow::select_row(int row) {
  if (row < 0 || row >= model_.rowCount()) {
    form_->setEnabled(false);
    return;
  }
  table_->setCurrentIndex(model_.index(row, 0));
  on_selection();
}

int ConditionalsEditorWindow::current_row() const {
  const QModelIndex i = table_->currentIndex();
  return i.isValid() ? i.row() : -1;
}

void ConditionalsEditorWindow::on_selection() {
  const int row = current_row();
  form_->setEnabled(editable_ && row >= 0);
  if (row < 0) return;
  const auto& c = model_.conditionals().items[static_cast<std::size_t>(row)];
  // An edit in the form comes back here through the model; what it shows is already this.
  if (!same_authored(form_->conditional(), c)) form_->set_conditional(c);
}

void ConditionalsEditorWindow::add_disable(const QString& name) {
  if (!editable_ || name.isEmpty()) return;
  QStringList names = model_.disable();
  if (names.contains(name)) return;
  names.append(name);
  model_.set_disable(names);
  fill_disable();
}

void ConditionalsEditorWindow::check_now() {
  check_timer_.stop();
  show_diagnostics(true);
}

void ConditionalsEditorWindow::show_diagnostics(bool with_catalog) {
  if (!editable_) return;
  diagnostics_->clear();
  auto add = [this](bool error, const QString& name, const QString& message) {
    auto* item = new QListWidgetItem(
        QStringLiteral("%1: %2: %3").arg(error ? QStringLiteral("error") : QStringLiteral("warning"), name, message),
        diagnostics_);
    item->setData(Qt::UserRole, name);
    item->setForeground(error ? theme().error_text : theme().warning_text);
  };
  ConditionalSet good;
  for (int row = 0; row < model_.rowCount(); ++row) {
    if (const QString e = model_.error(row); !e.isEmpty()) {
      add(true, model_.effective_name(row), e);
    } else if (auto f = experiment::finalize(model_.conditionals().items[static_cast<std::size_t>(row)])) {
      good.items.push_back(std::move(*f));
    }
  }
  if (!with_catalog) return;
  for (const auto& d : experiment::validate_conditionals(good, lab_.metric_catalog()))
    add(d.error, QString::fromStdString(d.conditional), QString::fromStdString(d.message));
}

QStringList ConditionalsEditorWindow::diagnostic_lines() const {
  QStringList out;
  for (int i = 0; i < diagnostics_->count(); ++i) out.append(diagnostics_->item(i)->text());
  return out;
}

QString ConditionalsEditorWindow::status_text() const { return status_->text(); }

void ConditionalsEditorWindow::update_title() {
  setWindowTitle(current_.isEmpty()
                     ? tr("Conditionals")
                     : tr("Conditionals - %1%2").arg(current_, modified() ? QStringLiteral(" *") : QString()));
}

void ConditionalsEditorWindow::closeEvent(QCloseEvent* event) {
  const QString was = current_;
  if (!resolve_unsaved()) {
    event->ignore();
    return;
  }
  // Discarded edits are gone the next time the window shows.
  if (model_.modified() || file_names().contains(was) == false) {
    if (file_names().contains(was)) load(was);
    else if (const QStringList names = file_names(); !names.isEmpty()) load(names.first());
    else show_nothing();
  }
  settings_->beginGroup(kSettingsGroup);
  settings_->setValue(QStringLiteral("geometry"), saveGeometry());
  settings_->setValue(QStringLiteral("last"), current_);
  settings_->endGroup();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
