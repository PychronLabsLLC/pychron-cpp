#include "script_editor_window.hpp"
#include "theme.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <fstream>
#include <sstream>

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

namespace fs = std::filesystem;
using experiment::lab::ScriptFile;
using scripting::ScriptKind;

constexpr ScriptKind kKinds[] = {ScriptKind::Extraction, ScriptKind::PostEquilibration, ScriptKind::PostMeasurement,
                                 ScriptKind::MeasurementHook};

QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString clock_text(double seconds) {
  const auto total = static_cast<long long>(std::llround(seconds));
  return QStringLiteral("%1:%2:%3")
      .arg(total / 3600)
      .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

std::optional<std::string> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

QString skeleton(ScriptKind kind) {
  switch (kind) {
    case ScriptKind::MeasurementHook:
      return QStringLiteral("# Measurement hook: define any of before_main, after_main, on_whiff_result.\n\n"
                            "def before_main(api):\n    pass\n");
    default:
      return QStringLiteral("#! pychron: \n\ndef main():\n    info('starting')\n");
  }
}

}  // namespace

ScriptEditorWindow::ScriptEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings,
                                       QWidget* parent)
    : QMainWindow(parent),
      lab_(lab),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      host_(scripting::make_script_host()),
      tree_(new QTreeWidget),
      tabs_(new QTabWidget),
      problems_(new QListWidget),
      status_(new QLabel) {
  setObjectName(QStringLiteral("ScriptEditorWindow"));
  setWindowTitle(tr("Script Editor"));
  resize(1100, 750);

  tree_->setHeaderHidden(true);
  auto* tree_dock = new QDockWidget(tr("Scripts"), this);
  tree_dock->setObjectName(QStringLiteral("ScriptEditorTreeDock"));
  tree_dock->setWidget(tree_);
  addDockWidget(Qt::LeftDockWidgetArea, tree_dock);

  tabs_->setTabsClosable(true);
  tabs_->setDocumentMode(true);
  status_->setWordWrap(true);
  auto* bottom = new QWidget;
  auto* bcol = new QVBoxLayout(bottom);
  bcol->setContentsMargins(0, 0, 0, 0);
  bcol->addWidget(status_);
  bcol->addWidget(problems_);
  auto* split = new QSplitter(Qt::Vertical);
  split->addWidget(tabs_);
  split->addWidget(bottom);
  split->setStretchFactor(0, 4);
  split->setStretchFactor(1, 1);
  split->setSizes({600, 160});  // an empty tab widget would otherwise get almost nothing
  setCentralWidget(split);

  auto* file = menuBar()->addMenu(tr("&Script"));
  auto add = [this](QMenu* menu, const QString& text, const QKeySequence& key, std::function<void()> f) {
    QAction* a = menu->addAction(text);
    if (!key.isEmpty()) a->setShortcut(key);
    connect(a, &QAction::triggered, this, [f = std::move(f)] { f(); });
    return a;
  };
  add(file, tr("&New..."), QKeySequence::New, [this] {
    QStringList kinds;
    for (auto k : kKinds) kinds.append(q(scripting::to_string(k)));
    bool ok = false;
    const QString kind = QInputDialog::getItem(this, tr("New script"), tr("Kind"), kinds, 0, false, &ok);
    if (!ok) return;
    const QString name = QInputDialog::getText(this, tr("New script"), tr("Name (e.g. co2_degas or co2:degas)"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QString error;
    if (!new_script(kKinds[std::max<qsizetype>(0, kinds.indexOf(kind))], name.trimmed(), &error))
      QMessageBox::warning(this, tr("New script"), error);
  });
  add(file, tr("&Save"), QKeySequence::Save, [this] {
    QString error;
    if (current() != nullptr && !save(&error)) QMessageBox::warning(this, tr("Save"), error);
  });
  add(file, tr("&Close Tab"), QKeySequence::Close, [this] { close_current(); });
  file->addSeparator();
  preferences_ = add(file, tr("Preferences…"), QKeySequence::Preferences, [this] {
    if (on_preferences_) on_preferences_(this);
  });
  preferences_->setMenuRole(QAction::PreferencesRole);
  preferences_->setVisible(false);
  auto* code = menuBar()->addMenu(tr("&Code"));
  add(code, tr("&Check Now"), QKeySequence(Qt::Key_F7), [this] { check_now(); });
  add(code, tr("Go to &Gosub"), QKeySequence(Qt::Key_F2), [this] {
    if (Document* d = current())
      if (auto name = d->editor->gosub_under_cursor()) follow_gosub(*name);
  });

  ask_unsaved_ = [this](const QString& name) {
    const auto b = QMessageBox::question(this, tr("Unsaved script"), tr("%1 has unsaved changes. Save them?").arg(name),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                         QMessageBox::Cancel);
    return b == QMessageBox::Save ? Unsaved::Save : b == QMessageBox::Discard ? Unsaved::Discard : Unsaved::Cancel;
  };

  check_timer_.setSingleShot(true);
  check_timer_.setInterval(kCheckDelayMs);
  connect(&check_timer_, &QTimer::timeout, this, [this] { check_now(); });
  connect(tabs_, &QTabWidget::currentChanged, this, [this] { show_check(); });
  connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) { close_document(i); });
  connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
    const QVariant path = item->data(0, Qt::UserRole);
    if (!path.isValid()) return;
    for (const auto& f : experiment::lab::lab_scripts(lab_))
      if (q(f.path.string()) == path.toString()) open(f);
  });
  connect(problems_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
    if (Document* d = current()) d->editor->go_to_line(item->data(Qt::UserRole).toInt());
  });

  fill_tree();
  show_check();
  settings_->beginGroup(QStringLiteral("script_editor"));
  if (auto g = settings_->value(QStringLiteral("geometry")).toByteArray(); !g.isEmpty()) restoreGeometry(g);
  settings_->endGroup();
}

ScriptEditorWindow::~ScriptEditorWindow() = default;

void ScriptEditorWindow::set_preferences_handler(std::function<void(QWidget*)> handler) {
  on_preferences_ = std::move(handler);
  preferences_->setVisible(static_cast<bool>(on_preferences_));
}

QString ScriptEditorWindow::label(const ScriptFile& file) { return q(scripting::to_string(file.kind)) + QLatin1Char('/') + q(file.name); }

void ScriptEditorWindow::fill_tree() {
  tree_->clear();
  std::map<QString, QTreeWidgetItem*> groups;
  for (auto k : kKinds) {
    auto* g = new QTreeWidgetItem(tree_, {q(scripting::to_string(k))});
    groups[q(scripting::to_string(k))] = g;
  }
  auto* lib = new QTreeWidgetItem(tree_, {QStringLiteral("lib")});
  for (const auto& f : experiment::lab::lab_scripts(lab_)) {
    const bool in_lib = f.name.rfind("lib:", 0) == 0;
    QTreeWidgetItem* parent = in_lib ? lib : groups[q(scripting::to_string(f.kind))];
    auto* item = new QTreeWidgetItem(parent, {q(in_lib ? f.name.substr(4) : f.name)});
    item->setData(0, Qt::UserRole, q(f.path.string()));
    item->setToolTip(0, q(f.path.string()));
  }
  tree_->expandAll();
}

QStringList ScriptEditorWindow::script_names() const {
  QStringList out;
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    const auto* g = tree_->topLevelItem(i);
    for (int j = 0; j < g->childCount(); ++j) out.append(g->text(0) + QLatin1Char('/') + g->child(j)->text(0));
  }
  return out;
}

ScriptEditorWindow::Document* ScriptEditorWindow::current() const {
  const int i = tabs_->currentIndex();
  return i >= 0 && i < static_cast<int>(docs_.size()) ? docs_[static_cast<std::size_t>(i)].get() : nullptr;
}

ScriptEditorWindow::Document* ScriptEditorWindow::find(const ScriptFile& file) const {
  std::error_code ec;
  for (const auto& d : docs_)
    if (fs::equivalent(d->file.path, file.path, ec)) return d.get();
  return nullptr;
}

bool ScriptEditorWindow::open(const ScriptFile& file) {
  if (Document* d = find(file)) {
    for (std::size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i].get() == d) tabs_->setCurrentIndex(static_cast<int>(i));
    return true;
  }
  auto text = read_file(file.path);
  if (!text) return false;
  auto doc = std::make_unique<Document>();
  doc->file = file;
  doc->editor = new CodeEditor;
  doc->editor->setPlainText(QString::fromStdString(*text));
  doc->editor->document()->setModified(false);
  doc->highlighter = new ScriptHighlighter(doc->editor->document(), file.kind);
  QStringList words;
  for (const auto& w : experiment::lab::completion_words(file.kind)) words.append(q(w));
  doc->editor->set_completion_words(words);
  Document* d = doc.get();
  // The document's own flag: the highlighter's formatting passes also emit
  // textChanged, so that signal says nothing about edits.
  connect(doc->editor->document(), &QTextDocument::modificationChanged, this, [this, d](bool changed) {
    d->modified = changed;
    update_tab_title(*d);
  });
  connect(doc->editor, &QPlainTextEdit::textChanged, this, [this, d] {
    if (current() == d && d->editor->toPlainText() != d->checked_text) check_timer_.start();
  });
  connect(doc->editor, &CodeEditor::gosubActivated, this, [this](const QString& name) { follow_gosub(name); });
  docs_.push_back(std::move(doc));
  const int index = tabs_->addTab(d->editor, QString());
  tabs_->setTabToolTip(index, q(file.path.string()));
  update_tab_title(*d);
  tabs_->setCurrentIndex(index);
  check_now();
  return true;
}

bool ScriptEditorWindow::open(ScriptKind kind, const QString& name) {
  auto path = experiment::lab::script_path(lab_, kind, name.toStdString());
  if (!path) return false;
  return open(ScriptFile{kind, name.toStdString(), *path});
}

bool ScriptEditorWindow::new_script(ScriptKind kind, const QString& name, QString* error) {
  auto set_error = [&](const QString& e) {
    if (error) *error = e;
    return false;
  };
  auto path = experiment::lab::script_path(lab_, kind, name.toStdString());
  if (!path) return set_error(q(path.error().what));
  std::error_code ec;
  if (fs::exists(*path, ec)) return set_error(tr("%1 already exists").arg(q(path->string())));
  fs::create_directories(path->parent_path(), ec);
  {
    std::ofstream out(*path, std::ios::binary);
    out << skeleton(kind).toStdString();
    if (!out) return set_error(tr("cannot write %1").arg(q(path->string())));
  }
  fill_tree();
  emit scriptsChanged();
  return open(ScriptFile{kind, name.toStdString(), *path});
}

int ScriptEditorWindow::document_count() const { return static_cast<int>(docs_.size()); }
QString ScriptEditorWindow::current_name() const { return current() ? label(current()->file) : QString(); }
CodeEditor* ScriptEditorWindow::current_editor() const { return current() ? current()->editor : nullptr; }
bool ScriptEditorWindow::modified() const { return current() != nullptr && current()->modified; }

bool ScriptEditorWindow::save(QString* error) {
  Document* d = current();
  if (d == nullptr) return false;
  const fs::path tmp = d->file.path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out << d->editor->toPlainText().toStdString();
    if (!out) {
      if (error) *error = tr("cannot write %1").arg(q(tmp.string()));
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, d->file.path, ec);
  if (ec) {
    if (error) *error = q(ec.message());
    return false;
  }
  d->editor->document()->setModified(false);  // clears `modified` through modificationChanged
  emit scriptsChanged();
  return true;
}

bool ScriptEditorWindow::resolve_unsaved(Document& doc) {
  if (!doc.modified) return true;
  switch (ask_unsaved_(label(doc.file))) {
    case Unsaved::Save: {
      for (std::size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i].get() == &doc) tabs_->setCurrentIndex(static_cast<int>(i));
      QString error;
      if (save(&error)) return true;
      QMessageBox::warning(this, tr("Save"), error);
      return false;
    }
    case Unsaved::Discard: return true;
    case Unsaved::Cancel: return false;
  }
  return false;
}

bool ScriptEditorWindow::close_document(int index) {
  if (index < 0 || index >= static_cast<int>(docs_.size())) return false;
  Document& d = *docs_[static_cast<std::size_t>(index)];
  if (!resolve_unsaved(d)) return false;
  tabs_->removeTab(index);
  d.editor->deleteLater();
  docs_.erase(docs_.begin() + index);
  show_check();
  return true;
}

bool ScriptEditorWindow::close_current() { return close_document(tabs_->currentIndex()); }

bool ScriptEditorWindow::follow_gosub(const QString& name) {
  Document* d = current();
  if (d == nullptr) return false;
  auto target = experiment::lab::resolve_gosub(lab_, d->file.kind, name.toStdString());
  if (!target) {
    status_->setText(tr("gosub '%1' does not resolve to a script").arg(name));
    return false;
  }
  return open(*target);
}

void ScriptEditorWindow::check_now() {
  check_timer_.stop();
  Document* d = current();
  if (d == nullptr) return;
  d->checked_text = d->editor->toPlainText();
  d->check = experiment::lab::check_script(*host_, lab_, d->file.kind, d->file.name, d->checked_text.toStdString());
  std::vector<EditorDiagnostic> marks;
  for (const auto& diag : d->check.report.diagnostics)
    if (diag.script.empty() || diag.script == d->file.name)
      marks.push_back({diag.line, diag.severity == scripting::Diagnostic::Severity::Error, q(diag.message)});
  d->editor->set_diagnostics(std::move(marks));
  show_check();
}

void ScriptEditorWindow::show_check() {
  problems_->clear();
  Document* d = current();
  if (d == nullptr) {
    status_->setText(tr("Open a script from the list, or Script > New."));
    style::set_tone(status_, style::Tone::Normal);
    return;
  }
  auto listed = d->check.report.diagnostics;  // in line order, gosub scripts after this one
  std::stable_sort(listed.begin(), listed.end(), [&](const auto& a, const auto& b) {
    const bool a_here = a.script.empty() || a.script == d->file.name;
    const bool b_here = b.script.empty() || b.script == d->file.name;
    return a_here != b_here ? a_here : a.line < b.line;
  });
  for (const auto& diag : listed) {
    const bool error = diag.severity == scripting::Diagnostic::Severity::Error;
    QString text = QStringLiteral("%1: %2: %3").arg(diag.line).arg(error ? tr("error") : tr("warning"), q(diag.message));
    if (!diag.script.empty() && diag.script != d->file.name) text = q(diag.script) + QStringLiteral(" ") + text;
    if (!diag.code.empty()) text += QStringLiteral(" [") + q(diag.code) + QStringLiteral("]");
    auto* item = new QListWidgetItem(text, problems_);
    item->setData(Qt::UserRole, diag.line);
    item->setForeground(error ? theme().error_text : theme().warning_text);
  }
  QString text;
  bool bad = false;
  if (!d->check.error.empty()) {
    text = tr("Not checked: %1").arg(q(d->check.error));
    bad = true;
  } else if (!d->check.report.ok()) {
    text = tr("%n error(s)", nullptr, static_cast<int>(d->check.report.errors().size()));
    bad = true;
  } else if (d->check.estimate) {
    const auto& e = *d->check.estimate;
    text = tr("Estimate %1").arg(clock_text(std::chrono::duration<double>(e.total).count()));
    if (!e.bounded()) text += tr(" at least (%1)").arg(q(e.unbounded.front()));
    if (const auto w = d->check.report.warnings().size(); w > 0) text += tr(" · %n warning(s)", nullptr, static_cast<int>(w));
  }
  status_->setText(text);
  style::set_tone(status_, bad ? style::Tone::Error : style::Tone::Normal);
}

QStringList ScriptEditorWindow::diagnostic_lines() const {
  QStringList out;
  for (int i = 0; i < problems_->count(); ++i) out.append(problems_->item(i)->text());
  return out;
}

QString ScriptEditorWindow::estimate_text() const { return status_->text(); }

void ScriptEditorWindow::update_tab_title(Document& doc) {
  for (std::size_t i = 0; i < docs_.size(); ++i)
    if (docs_[i].get() == &doc)
      tabs_->setTabText(static_cast<int>(i), q(doc.file.name) + (doc.modified ? QStringLiteral(" *") : QString()));
}

void ScriptEditorWindow::closeEvent(QCloseEvent* event) {
  for (auto& d : docs_) {
    if (!resolve_unsaved(*d)) {
      event->ignore();
      return;
    }
  }
  settings_->beginGroup(QStringLiteral("script_editor"));
  settings_->setValue(QStringLiteral("geometry"), saveGeometry());
  settings_->endGroup();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
