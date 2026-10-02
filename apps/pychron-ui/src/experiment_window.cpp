#include "experiment_window.hpp"

#include <algorithm>
#include <set>

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "pychron/experiment/model/queue_file.hpp"
#include "strip_chart_model.hpp"

namespace pychron::ui {

namespace {

namespace exec = experiment::executor;

std::map<std::string, QColor> detector_colors(const experiment::lab::Lab& lab) {
  std::map<std::string, QColor> out;
  if (!lab.spectrometer) return out;
  const auto& detectors = lab.spectrometer->config.detectors;
  for (std::size_t i = 0; i < detectors.size(); ++i) {
    QColor c(QString::fromStdString(detectors[i].color));
    out[detectors[i].name] = c.isValid() ? c : StripChartModel::palette_color(i);
  }
  return out;
}

}  // namespace

ExperimentWindow::ExperimentWindow(ExperimentBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings,
                                   QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      simulation_(simulation),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      model_(bridge.lab().ids, [&bridge](const experiment::QueueSpec& q) { return bridge.check(q); }),
      table_(new QTableView),
      diagnostics_(new QLabel),
      pane_(new ExecutorPane(bridge)),
      evolutions_(new EvolutionsView(detector_colors(bridge.lab()))),
      factory_(new RunFactoryPanel(bridge.lab(), model_, [this] { return selected_rows(); })),
      measurement_(new MeasurementPanel(bridge.lab(), model_, [this] { return selected_rows(); })) {
  setObjectName(QStringLiteral("ExperimentWindow"));
  resize(1300, 850);

  table_->setModel(&model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                          QAbstractItemView::AnyKeyPressed);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setColumnWidth(QueueTableModel::Row, 40);
  table_->setColumnWidth(QueueTableModel::Status, 110);
  table_->setContextMenuPolicy(Qt::ActionsContextMenu);
  diagnostics_->setObjectName(QStringLiteral("QueueDiagnostics"));
  diagnostics_->setStyleSheet(QStringLiteral("#QueueDiagnostics { background: #f8d7da; color: #721c24; padding: 4px; }"));
  diagnostics_->setWordWrap(true);
  diagnostics_->hide();

  auto* centre = new QWidget;
  auto* column = new QVBoxLayout(centre);
  column->setContentsMargins(0, 0, 0, 0);
  column->addWidget(diagnostics_);
  column->addWidget(table_, 1);
  setCentralWidget(centre);

  auto* executor_dock = new QDockWidget(tr("Executor"), this);
  executor_dock->setObjectName(QStringLiteral("ExperimentExecutorDock"));
  executor_dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  executor_dock->setWidget(pane_);
  addDockWidget(Qt::BottomDockWidgetArea, executor_dock);
  auto* evolutions_dock = new QDockWidget(tr("Evolutions"), this);
  evolutions_dock->setObjectName(QStringLiteral("ExperimentEvolutionsDock"));
  evolutions_dock->setWidget(evolutions_);
  addDockWidget(Qt::RightDockWidgetArea, evolutions_dock);
  auto* factory_dock = new QDockWidget(tr("Run Factory"), this);
  factory_dock->setObjectName(QStringLiteral("ExperimentFactoryDock"));
  factory_dock->setWidget(factory_);
  addDockWidget(Qt::LeftDockWidgetArea, factory_dock);
  auto* measurement_dock = new QDockWidget(tr("Measurement"), this);
  measurement_dock->setObjectName(QStringLiteral("ExperimentMeasurementDock"));
  measurement_dock->setWidget(measurement_);
  tabifyDockWidget(factory_dock, measurement_dock);
  factory_dock->raise();
  resizeDocks({evolutions_dock, factory_dock}, {500, 380}, Qt::Horizontal);  // saved state, if any, wins below
  resizeDocks({executor_dock}, {300}, Qt::Vertical);

  ask_unsaved_ = [this] {
    const auto b = QMessageBox::question(this, tr("Unsaved changes"), tr("The queue has unsaved changes. Save them?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                         QMessageBox::Cancel);
    return b == QMessageBox::Save ? Unsaved::Save : b == QMessageBox::Discard ? Unsaved::Discard : Unsaved::Cancel;
  };
  ask_stop_ = [this] {
    return QMessageBox::question(this, tr("Queue running"),
                                 tr("A queue is running. Stop it after the current run? (No keeps it running.)"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
  };

  build_actions();

  connect(&model_, &QueueTableModel::edited, this, [this] { set_modified(true); });
  connect(&model_, &QueueTableModel::validated, this, [this] { update_state(); });
  connect(pane_, &ExecutorPane::startRequested, this, [this] { start(); });
  connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, measurement_,
          [this] { measurement_->refresh(); });
  connect(factory_, &RunFactoryPanel::inserted, this, [this](const std::vector<std::size_t>& rows) {
    select_rows(rows);
    if (!rows.empty()) table_->scrollTo(model_.index(static_cast<int>(rows.back()), 0));
  });

  connect(&bridge_, &ExperimentBridge::runStarted, this, [this](const exec::RunStarted& e) {
    model_.on_run_started(e);
    evolutions_->on_run_started(e);
  });
  connect(&bridge_, &ExperimentBridge::runStateChanged, &model_, &QueueTableModel::on_run_state);
  connect(&bridge_, &ExperimentBridge::runFinished, &model_, &QueueTableModel::on_run_finished);
  connect(&bridge_, &ExperimentBridge::seriesUpdated, evolutions_, &EvolutionsView::on_series);
  connect(&bridge_, &ExperimentBridge::fitsUpdated, evolutions_, &EvolutionsView::on_fits);
  connect(&bridge_, &ExperimentBridge::peakCenterDone, evolutions_, &EvolutionsView::on_peak_center);
  connect(&bridge_, &ExperimentBridge::queueEdited, this, [this](const exec::QueueEdited& e) {
    if (model_.on_queue_edited(e)) set_modified(true);  // the file no longer matches the queue
  });
  connect(&bridge_, &ExperimentBridge::queueFrontier, this,
          [this](const exec::QueueFrontier& e) { model_.set_frozen(e.frozen); });
  connect(&model_, &QueueTableModel::editRefused, this,
          [this](const QString& why) { pane_->show_error(tr("Edit refused: %1").arg(why)); });
  connect(&bridge_, &ExperimentBridge::queueEnded, this, [this] {
    model_.end_live();
    update_state();
  });

  settings_->beginGroup(QStringLiteral("experiment_window"));
  if (auto g = settings_->value(QStringLiteral("geometry")).toByteArray(); !g.isEmpty()) restoreGeometry(g);
  if (auto s = settings_->value(QStringLiteral("state")).toByteArray(); !s.isEmpty()) restoreState(s);
  settings_->endGroup();

  update_title();
  update_state();
}

void ExperimentWindow::build_actions() {
  // Plain addAction(text) plus connect: the (text, receiver, functor)
  // overloads are deprecated in recent Qt.
  auto add = [this](QMenu* menu, const QString& text, std::function<void()> f, const QKeySequence& key = {}) {
    QAction* a = menu->addAction(text);
    if (!key.isEmpty()) a->setShortcut(key);
    connect(a, &QAction::triggered, this, [f = std::move(f)] { f(); });
    return a;
  };

  auto* file = menuBar()->addMenu(tr("&Queue"));
  auto* bar = addToolBar(tr("Queue"));
  bar->setObjectName(QStringLiteral("ExperimentToolBar"));
  open_ = add(file, tr("&Open..."), [this] { open_dialog(); }, QKeySequence::Open);
  save_ = add(
      file, tr("&Save"),
      [this] {
        if (!path_) {
          save_as_dialog();
          return;
        }
        QString error;
        if (!save(&error)) QMessageBox::warning(this, tr("Save"), error);
      },
      QKeySequence::Save);
  save_as_ = add(file, tr("Save &As..."), [this] { save_as_dialog(); });
  revalidate_ = add(file, tr("&Revalidate"), [this] { model_.revalidate(); });
  bar->addAction(open_);
  bar->addAction(save_);
  bar->addAction(revalidate_);

  auto* rows = menuBar()->addMenu(tr("&Rows"));
  auto add_row_action = [&](const QString& name, const QKeySequence& key, std::function<void()> f) {
    QAction* a = add(rows, name, std::move(f), key);
    table_->addAction(a);
    row_actions_.push_back(a);
  };
  add_row_action(tr("Move Up"), QKeySequence(Qt::CTRL | Qt::Key_Up), [this] {
    std::vector<std::size_t> moved;
    if (model_.move_up(selected_rows(), &moved)) select_rows(moved);
  });
  add_row_action(tr("Move Down"), QKeySequence(Qt::CTRL | Qt::Key_Down), [this] {
    std::vector<std::size_t> moved;
    if (model_.move_down(selected_rows(), &moved)) select_rows(moved);
  });
  add_row_action(tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { model_.duplicate(selected_rows()); });
  add_row_action(tr("Delete"), QKeySequence::Delete, [this] { model_.remove(selected_rows()); });
  add_row_action(tr("Toggle Skip"), QKeySequence(Qt::CTRL | Qt::Key_K), [this] { model_.toggle_skip(selected_rows()); });
  add_row_action(tr("End After"), QKeySequence(Qt::CTRL | Qt::Key_E), [this] {
    auto r = selected_rows();
    if (r.size() == 1) model_.toggle_end_after(r.front());
  });

  add_row_action(tr("Edit Extraction Script"), {}, [this] { edit_row_script(scripting::ScriptKind::Extraction); });
  add_row_action(tr("Edit Post-Measurement Script"), {},
                 [this] { edit_row_script(scripting::ScriptKind::PostMeasurement); });

  auto* scripts = menuBar()->addMenu(tr("S&cripts"));
  add(scripts, tr("Script &Editor..."), [this] { open_script_editor(); }, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));

  auto* run = menuBar()->addMenu(tr("&Executor"));
  add(run, tr("Start"), [this] { pane_->request_start(); }, QKeySequence(Qt::Key_F5));
  add(run, tr("Stop"), [this] { pane_->request_stop(); });
  add(run, tr("Cancel..."), [this] { pane_->request_cancel(); });
  add(run, tr("Abort..."), [this] { pane_->request_abort(); });
  add(run, tr("Truncate"), [this] { pane_->request_truncate(); });
  run->addSeparator();
  add(run, tr("Send Test Notification"), [this] { bridge_.session().notify_test(); });
}

std::vector<std::size_t> ExperimentWindow::selected_rows() const {
  if (table_->selectionModel() == nullptr) return {};  // panels ask while the window is still being built
  std::set<std::size_t> rows;
  for (const auto& i : table_->selectionModel()->selectedIndexes()) rows.insert(static_cast<std::size_t>(i.row()));
  return {rows.begin(), rows.end()};
}

void ExperimentWindow::select_rows(const std::vector<std::size_t>& rows) {
  auto* sel = table_->selectionModel();
  sel->clearSelection();
  for (auto r : rows)
    sel->select(model_.index(static_cast<int>(r), 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

void ExperimentWindow::select_row(int row) { select_rows({static_cast<std::size_t>(row)}); }

ScriptEditorWindow* ExperimentWindow::open_script_editor() {
  if (script_editor_ == nullptr) {
    script_editor_ = new ScriptEditorWindow(bridge_.lab(), nullptr, this);
    script_editor_->setWindowFlag(Qt::Window);
    // A new or saved script may fix (or break) rows that name it.
    connect(script_editor_, &ScriptEditorWindow::scriptsChanged, this, [this] { model_.revalidate(); });
  }
  script_editor_->show();
  script_editor_->raise();
  script_editor_->activateWindow();
  return script_editor_;
}

bool ExperimentWindow::edit_row_script(scripting::ScriptKind kind) {
  const auto rows = selected_rows();
  if (rows.size() != 1 || rows.front() >= model_.queue().runs.size()) return false;
  const auto& run = model_.queue().runs[rows.front()];
  std::string name;
  if (kind == scripting::ScriptKind::Extraction) name = run.extraction.script;
  else if (kind == scripting::ScriptKind::PostMeasurement) name = run.post_measurement.value_or("");
  else if (kind == scripting::ScriptKind::PostEquilibration) name = run.post_equilibration.value_or("");
  if (name.empty()) return false;
  return open_script_editor()->open(kind, QString::fromStdString(name));
}

void ExperimentWindow::set_confirm(ExecutorPane::Confirm confirm) { pane_->set_confirm(std::move(confirm)); }

bool ExperimentWindow::resolve_unsaved() {
  if (!modified_) return true;
  switch (ask_unsaved_()) {
    case Unsaved::Save: {
      if (!path_) {
        save_as_dialog();
        return !modified_;
      }
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

bool ExperimentWindow::load_queue(const std::filesystem::path& path, QString* error) {
  auto set_error = [&](const QString& e) {
    if (error) *error = e;
    return false;
  };
  if (bridge_.running()) return set_error(tr("a queue is running"));
  if (!resolve_unsaved()) return set_error(tr("cancelled"));
  auto q = experiment::load_queue_file(path.string(), bridge_.lab().ids);
  if (!q) return set_error(QString::fromStdString(q.error().what));
  model_.set_queue(std::move(*q));
  path_ = path;
  set_modified(false);
  settings_->setValue(QStringLiteral("experiment_window/last_queue"), QString::fromStdString(path.string()));
  return true;
}

bool ExperimentWindow::save(QString* error) {
  if (!path_) {
    if (error) *error = tr("the queue has no file yet");
    return false;
  }
  return save_as(*path_, error);
}

bool ExperimentWindow::save_as(const std::filesystem::path& path, QString* error) {
  if (auto r = experiment::save_queue_file(path.string(), model_.queue()); !r) {
    if (error) *error = QString::fromStdString(r.error().what);
    return false;
  }
  path_ = path;
  set_modified(false);
  return true;
}

void ExperimentWindow::open_dialog() {
  if (bridge_.running()) {
    QMessageBox::information(this, tr("Open"), tr("A queue is running."));
    return;
  }
  const QString last = settings_->value(QStringLiteral("experiment_window/last_queue")).toString();
  const QString file = QFileDialog::getOpenFileName(this, tr("Open queue"), last, tr("Experiment queues (*.toml)"));
  if (file.isEmpty()) return;
  QString error;
  if (!load_queue(file.toStdString(), &error) && error != tr("cancelled"))
    QMessageBox::warning(this, tr("Open"), error);
}

void ExperimentWindow::save_as_dialog() {
  const QString start = path_ ? QString::fromStdString(path_->string()) : QString();
  const QString file = QFileDialog::getSaveFileName(this, tr("Save queue"), start, tr("Experiment queues (*.toml)"));
  if (file.isEmpty()) return;
  QString error;
  if (!save_as(file.toStdString(), &error)) QMessageBox::warning(this, tr("Save"), error);
}

void ExperimentWindow::start() {
  if (bridge_.running()) return;
  const auto rows = selected_rows();
  const std::size_t from = rows.empty() ? 0 : rows.front();
  auto r = bridge_.start(model_.queue(), from);
  if (!r) {
    pane_->show_error(QString::fromStdString(r.error().what));
    return;
  }
  model_.clear_status();
  // Rows after those the executor has reached stay editable; each change
  // goes to the running executor first.
  model_.set_live(from, [this](std::uint64_t base, const experiment::QueueSpec& q) { return bridge_.edit(base, q); });
  evolutions_->clear();
  pane_->set_running(true);
  update_state();
}

void ExperimentWindow::set_modified(bool modified) {
  modified_ = modified;
  update_title();
}

void ExperimentWindow::update_title() {
  QString title = tr("Experiment");
  if (simulation_) title += tr(" (Simulation)");
  if (path_) title += QStringLiteral(" — ") + QString::fromStdString(path_->filename().string());
  if (modified_) title += QStringLiteral(" *");
  setWindowTitle(title);
}

QString ExperimentWindow::queue_diagnostics_text() const { return diagnostics_->isHidden() ? QString() : diagnostics_->text(); }

void ExperimentWindow::update_state() {
  const QStringList queue_level = model_.queue_diagnostics();
  diagnostics_->setText(queue_level.join(QLatin1Char('\n')));
  diagnostics_->setVisible(!queue_level.isEmpty());
  int runnable_rows = 0;
  for (const auto& r : model_.queue().runs) runnable_rows += r.skip ? 0 : 1;
  pane_->set_runnable(model_.runnable(), runnable_rows);
  const bool running = bridge_.running() || pane_->running();
  open_->setEnabled(!running);
}

void ExperimentWindow::closeEvent(QCloseEvent* event) {
  if (!resolve_unsaved() || (script_editor_ != nullptr && script_editor_->isVisible() && !script_editor_->close())) {
    event->ignore();
    return;
  }
  if (bridge_.running() && ask_stop_()) bridge_.stop();
  settings_->beginGroup(QStringLiteral("experiment_window"));
  settings_->setValue(QStringLiteral("geometry"), saveGeometry());
  settings_->setValue(QStringLiteral("state"), saveState());
  settings_->endGroup();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
