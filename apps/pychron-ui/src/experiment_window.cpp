#include "experiment_window.hpp"

#include "menu_hub.hpp"
#include "shortcuts.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "pychron/experiment/model/queue_file.hpp"
#include "reference_series_dock.hpp"
#include "strip_chart_model.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

namespace exec = experiment::executor;

// The config's detector colours, and the palette slot (the config index) of
// each detector without one: that colour follows a change of the palette.
struct DetectorColors {
  std::map<std::string, QColor> colors;
  std::map<std::string, std::size_t> palette_slots;
};

DetectorColors detector_colors(const experiment::lab::Lab& lab) {
  DetectorColors out;
  if (!lab.spectrometer) return out;
  const auto& detectors = lab.spectrometer->config.detectors;
  for (std::size_t i = 0; i < detectors.size(); ++i) {
    QColor c(QString::fromStdString(detectors[i].color));
    if (c.isValid()) {
      out.colors[detectors[i].name] = c;
    } else {
      out.palette_slots[detectors[i].name] = i;
    }
  }
  return out;
}

EvolutionsView* make_evolutions(const experiment::lab::Lab& lab) {
  auto [colors, palette_slots] = detector_colors(lab);
  return new EvolutionsView(std::move(colors), std::move(palette_slots));  // the window's layout takes it
}

}  // namespace

ExperimentWindow::ExperimentWindow(ExperimentBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings,
                                   QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      simulation_(simulation),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      recent_(*settings_),
      model_(bridge.lab().ids, [&bridge](const experiment::QueueSpec& q) { return bridge.check(q); }),
      queue_table_(new QTableView),
      diagnostics_(new QLabel),
      pane_(new ExecutorPane(bridge)),
      evolutions_(make_evolutions(bridge.lab())),
      factory_(new RunFactoryPanel(bridge.lab(), model_, [this] { return selected_rows(); })),
      measurement_(new MeasurementPanel(bridge.lab(), model_, [this] { return selected_rows(); })) {
  setObjectName(QStringLiteral("ExperimentWindow"));
  resize(1300, 850);

  queue_table_->setObjectName(QStringLiteral("queue_table"));
  queue_table_->setModel(&queue_proxy_);
  queue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  queue_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  queue_table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                QAbstractItemView::AnyKeyPressed);
  queue_table_->verticalHeader()->hide();
  queue_table_->horizontalHeader()->setStretchLastSection(true);
  queue_table_->setContextMenuPolicy(Qt::ActionsContextMenu);

  // The executed runs: read-only, the Queue table's column widths.
  executed_table_ = new QTableView;
  executed_table_->setObjectName(QStringLiteral("executed_table"));
  executed_table_->setModel(&executed_proxy_);
  executed_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  executed_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  executed_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  executed_table_->verticalHeader()->hide();
  executed_table_->horizontalHeader()->setStretchLastSection(true);
  connect(queue_table_->horizontalHeader(), &QHeaderView::sectionResized, executed_table_,
          [this](int section, int, int size) { executed_table_->horizontalHeader()->resizeSection(section, size); });
  queue_table_->setColumnWidth(QueueTableModel::Row, 40);
  queue_table_->setColumnWidth(QueueTableModel::Status, 110);
  for (int c = 0; c < model_.columnCount(); ++c) executed_table_->setColumnWidth(c, queue_table_->columnWidth(c));
  // It follows its newest row while its scroll bar is at the end.
  connect(executed_table_->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) { executed_follow_ = value >= executed_table_->verticalScrollBar()->maximum(); });

  executed_fold_ = new QToolButton;
  executed_fold_->setObjectName(QStringLiteral("executed_fold"));
  executed_fold_->setAutoRaise(true);
  connect(executed_fold_, &QToolButton::clicked, this, [this] { set_executed_collapsed(!executed_collapsed_); });
  executed_count_ = new QLabel;
  executed_count_->setObjectName(QStringLiteral("executed_count"));
  executed_header_ = new QWidget;
  auto* executed_header_row = new QHBoxLayout(executed_header_);
  executed_header_row->setContentsMargins(6, 2, 6, 2);
  executed_header_row->addWidget(executed_fold_);
  executed_header_row->addWidget(executed_count_);
  executed_header_row->addStretch(1);
  executed_section_ = new QWidget;
  executed_section_->setObjectName(QStringLiteral("executed_section"));
  auto* executed_column = new QVBoxLayout(executed_section_);
  executed_column->setContentsMargins(0, 0, 0, 0);
  executed_column->setSpacing(0);
  executed_column->addWidget(executed_header_);
  executed_column->addWidget(executed_table_, 1);

  tables_splitter_ = new QSplitter(Qt::Vertical);
  tables_splitter_->setObjectName(QStringLiteral("tables_splitter"));
  tables_splitter_->setChildrenCollapsible(false);
  tables_splitter_->addWidget(executed_section_);
  tables_splitter_->addWidget(queue_table_);
  tables_splitter_->setStretchFactor(0, 1);
  tables_splitter_->setStretchFactor(1, 2);
  // The fold first: a collapsed section has only its header's minimum
  // height, and the sizes restored must not be held to the table's.
  executed_collapsed_ = settings_->value(QStringLiteral("experiment_window/executed_collapsed"), false).toBool();
  set_executed_collapsed(executed_collapsed_);
  const QString split_key = QStringLiteral("experiment_window/tables_splitter");
  if (settings_->contains(split_key)) tables_splitter_->restoreState(settings_->value(split_key).toByteArray());
  connect(tables_splitter_, &QSplitter::splitterMoved, this,
          [this, split_key] { settings_->setValue(split_key, tables_splitter_->saveState()); });
  diagnostics_->setObjectName(QStringLiteral("QueueDiagnostics"));
  style::make_banner(diagnostics_);
  diagnostics_->setWordWrap(true);
  diagnostics_->hide();

  auto* center = new QWidget;
  auto* column = new QVBoxLayout(center);
  column->setContentsMargins(0, 0, 0, 0);
  column->addWidget(diagnostics_);
  queue_conditionals_ = new QComboBox;
  queue_conditionals_->setObjectName(QStringLiteral("queue_conditionals"));
  queue_conditionals_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  queue_conditionals_->setToolTip(tr("Conditionals checked for every run of this queue, with the lab's system file"));
  auto* edit_conditionals = new QPushButton(tr("Edit..."));
  auto* header = new QHBoxLayout;
  header->setContentsMargins(6, 2, 6, 2);
  header->addWidget(new QLabel(tr("Queue conditionals")));
  header->addWidget(queue_conditionals_);
  header->addWidget(edit_conditionals);
  header->addStretch(1);
  column->addLayout(header);
  column->addWidget(tables_splitter_, 1);
  connect(queue_conditionals_, &QComboBox::activated, this, [this](int i) {
    if (!model_.set_queue_conditionals(queue_conditionals_->itemData(i).toString().toStdString()))
      sync_queue_conditionals();  // refused: show what the queue has
  });
  connect(edit_conditionals, &QPushButton::clicked, this,
          [this] { open_conditionals_editor(queue_conditionals_->currentData().toString()); });
  pick_conditionals_ = [this](const QStringList& names, const QList<Qt::CheckState>& states) {
    return pick_conditionals_dialog(names, states);
  };
  setCentralWidget(center);

  // How many runs the timeline follows is the operator's, kept with the window.
  const QString follow_key = QStringLiteral("experiment_window/timeline_follow_runs");
  pane_->timeline_view()->set_follow_runs(settings_->value(follow_key, TimelineView::kFollowActive).toInt());
  connect(pane_->timeline_view(), &TimelineView::follow_runs_changed, this,
          [this, follow_key](int runs) { settings_->setValue(follow_key, runs); });

  executor_dock_ = new QDockWidget(tr("Executor"), this);
  executor_dock_->setObjectName(QStringLiteral("ExperimentExecutorDock"));
  executor_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  executor_dock_->setWidget(pane_);
  evolutions_dock_ = new QDockWidget(tr("Evolutions"), this);
  evolutions_dock_->setObjectName(QStringLiteral("ExperimentEvolutionsDock"));
  evolutions_dock_->setWidget(evolutions_);
  factory_dock_ = new QDockWidget(tr("Run Factory"), this);
  factory_dock_->setObjectName(QStringLiteral("ExperimentFactoryDock"));
  factory_dock_->setWidget(factory_);
  measurement_dock_ = new QDockWidget(tr("Measurement"), this);
  measurement_dock_->setObjectName(QStringLiteral("ExperimentMeasurementDock"));
  measurement_dock_->setWidget(measurement_);

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
  pick_open_file_ = [this](const QString& start) {
    return QFileDialog::getOpenFileName(this, tr("Open queue"), start, tr("Experiment queues (*.toml)"));
  };
  pick_save_file_ = [this](const QString& start) {
    return QFileDialog::getSaveFileName(this, tr("Save queue"), start, tr("Experiment queues (*.toml)"));
  };

  ask_clear_executed_ = [this](int runs) {
    return QMessageBox::question(this, tr("Clear Executed"), tr("Remove %1 executed runs from the queue?").arg(runs),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
  };

  ask_resume_ = [this](const ResumeOffer& offer) { return ask_resume_dialog(offer); };
  inform_ = [this](const QString& text) { QMessageBox::information(this, tr("Queue"), text); };

  build_actions();

  connect(&model_, &QueueTableModel::edited, this, [this] { set_modified(true); });
  connect(&model_, &QueueTableModel::validated, this, [this] { update_state(); });
  connect(pane_, &ExecutorPane::startRequested, this, [this] { start(); });
  connect(&model_, &QueueTableModel::executedChanged, this, [this] { on_executed_changed(); });
  connect(queue_table_->selectionModel(), &QItemSelectionModel::selectionChanged, measurement_,
          [this] { measurement_->refresh(); });
  connect(factory_, &RunFactoryPanel::inserted, this, [this](const std::vector<std::size_t>& rows) {
    select_rows(rows);
    if (rows.empty()) return;
    const QModelIndex at = queue_proxy_.mapFromSource(model_.index(static_cast<int>(rows.back()), 0));
    if (at.isValid()) queue_table_->scrollTo(at);
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
  connect(&bridge_, &ExperimentBridge::peakCenterTryStarted, evolutions_, &EvolutionsView::on_peak_center_try);
  connect(&bridge_, &ExperimentBridge::peakCenterPoint, evolutions_, &EvolutionsView::on_peak_center_point);
  // The peak center graph keeps the queue's results: a new queue starts it empty.
  connect(&bridge_, &ExperimentBridge::queueStarted, evolutions_, [this] { evolutions_->clear_peak_centers(); });
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

  // What the References dock asked to have done later: looked at as often as
  // the timeline ticks, against the same clock.
  later_clock_.setInterval(250);
  connect(&later_clock_, &QTimer::timeout, this, [this] { run_due(); });

  default_layout();  // the saved one, if any, wins below
  layouts_ = new DockLayouts(this, [this] { default_layout(); }, settings_.get(), QStringLiteral("experiment_window"));
  layouts_->restore_last();

  update_title();
  on_executed_changed();  // ends with update_state()
}

// The panels and the toolbar as installed. Run again by Reset Layout, so it
// undoes whatever was done to them.
void ExperimentWindow::default_layout() {
  for (QDockWidget* dock : {executor_dock_, evolutions_dock_, factory_dock_, measurement_dock_}) dock->setFloating(false);
  addDockWidget(Qt::BottomDockWidgetArea, executor_dock_);
  addDockWidget(Qt::RightDockWidgetArea, evolutions_dock_);
  addDockWidget(Qt::LeftDockWidgetArea, factory_dock_);
  addDockWidget(Qt::LeftDockWidgetArea, measurement_dock_);
  tabifyDockWidget(factory_dock_, measurement_dock_);
  for (QDockWidget* dock : {executor_dock_, evolutions_dock_, factory_dock_, measurement_dock_}) dock->show();
  factory_dock_->raise();
  if (references_dock_ != nullptr) {
    // Behind the evolutions, which stay in front.
    references_dock_->setFloating(false);
    addDockWidget(Qt::RightDockWidgetArea, references_dock_);
    tabifyDockWidget(evolutions_dock_, references_dock_);
    references_dock_->show();
    evolutions_dock_->raise();
  }
  resizeDocks({evolutions_dock_, factory_dock_}, {500, 380}, Qt::Horizontal);
  resizeDocks({executor_dock_}, {300}, Qt::Vertical);
  addToolBar(Qt::TopToolBarArea, toolbar_);
  toolbar_->show();
}

void ExperimentWindow::set_references(ProcessingBridge* bridge, processing::PresetStore* presets) {
  if (references_dock_ != nullptr) {
    // Its reads again after a saved run go with it; the dock lets go of its
    // channels on the bridge it was given, which is still there.
    later_.clear();
    later_clock_.stop();
    removeDockWidget(references_dock_);
    delete references_dock_;
    references_dock_ = nullptr;
    references_ = nullptr;
  }
  if (bridge == nullptr || presets == nullptr) return;

  ReferenceSeriesDock::Deps deps;
  deps.bridge = bridge;
  deps.presets = presets;
  deps.wall_now = [this] {
    return std::chrono::duration<double>(bridge_.session().wall_now().time_since_epoch()).count();
  };
  deps.mass_spectrometer = [this] { return model_.queue().mass_spectrometer; };
  deps.settings = settings_.get();
  deps.later = [this](std::chrono::seconds delay, std::function<void()> what) { later(delay, std::move(what)); };
  references_ = new ReferenceSeriesDock(std::move(deps));
  references_dock_ = new QDockWidget(tr("References"), this);
  references_dock_->setObjectName(QStringLiteral("ExperimentReferencesDock"));
  references_dock_->setWidget(references_);
  // The window's last layout was applied before there was this dock. Qt kept
  // what that layout said of it, if anything: a layout saved with the dock
  // puts it back where it was left (once; a dock made again later, for
  // another data source, finds nothing kept). One saved without it, or none,
  // leaves it where the factory layout has it, behind the evolutions.
  if (!restoreDockWidget(references_dock_)) {
    addDockWidget(Qt::RightDockWidgetArea, references_dock_);
    tabifyDockWidget(evolutions_dock_, references_dock_);
    evolutions_dock_->raise();
  }
  // Queued: the run's save is done with before the dock reads for it. A run
  // that was not saved has nothing new to show.
  connect(
      &bridge_, &ExperimentBridge::runFinished, references_,
      [this](const exec::RunFinished& e) {
        if (!e.summary.save_error && references_ != nullptr) references_->run_saved();
      },
      Qt::QueuedConnection);
}

void ExperimentWindow::references_follow(const std::string& before) {
  // The dock draws the queue's spectrometer: another queue may have another.
  if (references_ != nullptr && model_.queue().mass_spectrometer != before) references_->read();
}

void ExperimentWindow::later(std::chrono::seconds delay, std::function<void()> what) {
  later_.push_back(Later{.at = bridge_.session().now() + delay, .what = std::move(what)});
  if (!later_clock_.isActive()) later_clock_.start();
}

void ExperimentWindow::run_due() {
  const TimePoint now = bridge_.session().now();
  std::vector<std::function<void()>> due;
  for (auto it = later_.begin(); it != later_.end();) {
    if (it->at <= now) {
      due.push_back(std::move(it->what));
      it = later_.erase(it);
    } else {
      ++it;
    }
  }
  if (later_.empty()) later_clock_.stop();
  for (auto& what : due) what();  // may ask for more
}

void ExperimentWindow::build_actions() {
  // The actions go into the unified menu bar (MenuHub), enabled while this
  // window is active; `into` collects each menu's group.
  using Menu = MenuHub::Menu;
  std::map<Menu, QList<QAction*>> into;
  auto add = [this, &into](Menu menu, const QString& text, std::function<void()> f, const QKeySequence& key = {}) {
    auto* a = new QAction(text, this);
    if (!key.isEmpty()) a->setShortcut(key);
    connect(a, &QAction::triggered, this, [f = std::move(f)] { f(); });
    into[menu].append(a);
    return a;
  };

  const Menu file = Menu::Queue;
  toolbar_ = addToolBar(tr("Queue"));
  QToolBar* bar = toolbar_;
  bar->setObjectName(QStringLiteral("ExperimentToolBar"));
  // Save and Save As are the File menu's (MenuHub), which has their keys:
  // these are what it does while this window is in front. File > New and
  // File > Open are the main window's, and call new_dialog() and
  // open_dialog(); Open is here too for the toolbar.
  auto file_action = [this](MenuHub::FileRole role, const QString& text, std::function<void()> f) {
    auto* a = new QAction(text, this);
    connect(a, &QAction::triggered, this, [f = std::move(f)] { f(); });
    MenuHub::instance().set_file_action(this, role, a, tr("Queue"));
    return a;
  };
  open_ = new QAction(tr("&Open..."), this);
  connect(open_, &QAction::triggered, this, [this] { open_dialog(); });
  save_ = file_action(MenuHub::FileRole::Save, tr("&Save"), [this] {
    if (!path_) {
      save_as_dialog();
      return;
    }
    QString error;
    if (!save(&error)) QMessageBox::warning(this, tr("Save"), error);
  });
  save_as_ = file_action(MenuHub::FileRole::SaveAs, tr("Save &As..."), [this] { save_as_dialog(); });
  revalidate_ = add(file, tr("&Revalidate"), [this] { model_.revalidate(); });
  // The executed runs (queue-tables design 4); each says why it refuses.
  add(file, tr("Reset"), [this] {
    QString e;
    if (!reset_executed(&e)) inform_refusal(e);
  });
  add(file, tr("Clear Executed..."), [this] {
    QString e;
    if (!clear_executed(&e)) inform_refusal(e);
  });
  auto repeat_last_run = [this] {
    QString e;
    if (!repeat_last(&e)) inform_refusal(e);
  };
  add(file, tr("Repeat Last Run"), repeat_last_run);
  // Resume after a crash or a Stop (5): says why when there is nothing.
  resume_ = add(file, tr("Resume..."), [this] {
    QString e;
    if (offer_resume(&e)) return;
    inform_(e == tr("a queue is running") ? tr("A queue is running.") : tr("Nothing to resume."));
  });
  bar->addAction(open_);
  bar->addAction(save_);
  bar->addAction(revalidate_);

  const Menu rows = Menu::Rows;
  auto add_row_action = [&](const QString& name, const QKeySequence& key, std::function<void()> f) {
    QAction* a = add(rows, name, std::move(f), key);
    queue_table_->addAction(a);
    row_actions_.push_back(a);
  };
  add_row_action(tr("Move Up"), key(Shortcut::MoveRowsUp), [this] {
    std::vector<std::size_t> moved;
    if (model_.move_up(selected_rows(), &moved)) select_rows(moved);
  });
  add_row_action(tr("Move Down"), key(Shortcut::MoveRowsDown), [this] {
    std::vector<std::size_t> moved;
    if (model_.move_down(selected_rows(), &moved)) select_rows(moved);
  });
  add_row_action(tr("Duplicate"), key(Shortcut::DuplicateRows), [this] { model_.duplicate(selected_rows()); });
  add_row_action(tr("Delete"), key(Shortcut::DeleteRows), [this] { model_.remove(selected_rows()); });
  add_row_action(tr("Toggle Skip"), key(Shortcut::ToggleSkip), [this] { model_.toggle_skip(selected_rows()); });
  add_row_action(tr("End After"), key(Shortcut::EndAfter), [this] {
    auto r = selected_rows();
    if (r.size() == 1) model_.toggle_end_after(r.front());
  });

  add_row_action(tr("Edit Extraction Script"), {}, [this] { edit_row_script(scripting::ScriptKind::Extraction); });
  add_row_action(tr("Edit Post-Measurement Script"), {},
                 [this] { edit_row_script(scripting::ScriptKind::PostMeasurement); });

  add_row_action(tr("Set Conditionals..."), {}, [this] { edit_selected_conditionals(); });

  // The Executed table's own menu: its rows are read-only, so only Repeat
  // and the scripts to look at.
  auto add_executed_action = [this](const QString& name, std::function<void()> f) {
    auto* a = new QAction(name, this);
    connect(a, &QAction::triggered, this, [f = std::move(f)] { f(); });
    executed_table_->addAction(a);
  };
  executed_table_->setContextMenuPolicy(Qt::ActionsContextMenu);
  add_executed_action(tr("Repeat"), repeat_last_run);
  auto open_executed_script = [this](scripting::ScriptKind kind) {
    const auto selected = selected_executed_rows();
    if (selected.size() == 1) open_row_script(selected.front(), kind);
  };
  add_executed_action(tr("Open Extraction Script"),
                      [open_executed_script] { open_executed_script(scripting::ScriptKind::Extraction); });
  add_executed_action(tr("Open Post-Measurement Script"),
                      [open_executed_script] { open_executed_script(scripting::ScriptKind::PostMeasurement); });

  add(Menu::Scripts, tr("Script &Editor..."), [this] { open_script_editor(); }, key(Shortcut::ScriptEditor));
  add(Menu::Scripts, tr("&Conditionals Editor..."), [this] { open_conditionals_editor(); });

  const Menu run = Menu::Executor;
  add(run, tr("Start"), [this] { pane_->request_start(); }, key(Shortcut::StartQueue));
  add(run, tr("Stop"), [this] { pane_->request_stop(); });
  add(run, tr("Cancel..."), [this] { pane_->request_cancel(); });
  add(run, tr("Abort..."), [this] { pane_->request_abort(); });
  add(run, tr("Truncate"), [this] { pane_->request_truncate(); });
  // File > Open Recent, after the hub's own four File commands.
  recent_menu_ = new QMenu(tr("Open &Recent"), this);
  connect(recent_menu_, &QMenu::aboutToShow, this, [this] { rebuild_recent(); });
  into[Menu::File].append(recent_menu_->menuAction());
  rebuild_recent();

  // While the queue holds: the pane says when they apply.
  resume_held_ = add(run, tr("Resume"), [this] { pane_->request_resume(); });
  skip_held_ = add(run, tr("Skip Held Run..."), [this] { pane_->request_skip(); });
  const auto follow_hold = [this] {
    resume_held_->setEnabled(pane_->resume_enabled());
    skip_held_->setEnabled(pane_->skip_enabled());
  };
  follow_hold();
  connect(pane_, &ExecutorPane::holdChanged, this, follow_hold);
  auto& menus = MenuHub::instance();
  for (const auto& [menu, actions] : into) menus.contribute(this, menu, actions, MenuHub::Scope::Window);
  // The test notification is apart from the run controls.
  auto* notify = new QAction(tr("Send Test Notification"), this);
  connect(notify, &QAction::triggered, this, [this] { bridge_.session().notify_test(); });
  menus.contribute(this, run, {notify}, MenuHub::Scope::Window);
}

void ExperimentWindow::rebuild_recent() {
  if (recent_menu_ == nullptr) return;  // not built yet
  // Rebuilt from inside an entry's own triggered() (opening or clearing
  // changes the list): the old actions go later, not under their signal.
  for (QAction* old : recent_menu_->actions()) {
    recent_menu_->removeAction(old);
    old->deleteLater();
  }
  const QStringList entries = recent_.entries();
  for (const QString& file : entries) {
    const QString name = QString::fromStdU16String(std::filesystem::path(file.toStdU16String()).filename().u16string());
    QAction* a = recent_menu_->addAction(name);
    a->setToolTip(file);
    a->setData(file);
    connect(a, &QAction::triggered, this, [this, file] { open_recent(file); });
  }
  recent_menu_->addSeparator();
  connect(recent_menu_->addAction(tr("Clear Menu")), &QAction::triggered, this, [this] {
    recent_.clear();
    rebuild_recent();
  });
  recent_menu_->menuAction()->setEnabled(!entries.isEmpty());
}

void ExperimentWindow::open_recent(const QString& file) {
  if (bridge_.running()) {
    QMessageBox::information(this, tr("Open"), tr("A queue is running."));
    return;
  }
  QString error;
  if (!load_queue(std::filesystem::path(file.toStdU16String()), &error) && error != tr("cancelled"))
    QMessageBox::warning(this, tr("Open"), error);
}

// Source rows of the Queue table's selection: the rows row actions act on.
std::vector<std::size_t> ExperimentWindow::selected_rows() const {
  if (queue_table_->selectionModel() == nullptr) return {};  // panels ask while the window is still being built
  std::set<std::size_t> rows;
  for (const auto& i : queue_table_->selectionModel()->selectedIndexes()) {
    const QModelIndex source = queue_proxy_.mapToSource(i);
    if (source.isValid()) rows.insert(static_cast<std::size_t>(source.row()));
  }
  return {rows.begin(), rows.end()};
}

// Selects source rows in the Queue table; one not in it (executed) is left out.
void ExperimentWindow::select_rows(const std::vector<std::size_t>& rows) {
  auto* sel = queue_table_->selectionModel();
  sel->clearSelection();
  for (auto r : rows) {
    const QModelIndex at = queue_proxy_.mapFromSource(model_.index(static_cast<int>(r), 0));
    if (at.isValid()) sel->select(at, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  }
}

void ExperimentWindow::select_row(int row) { select_rows({static_cast<std::size_t>(row)}); }

// Source rows of the Executed table's selection.
std::vector<std::size_t> ExperimentWindow::selected_executed_rows() const {
  std::set<std::size_t> rows;
  for (const auto& i : executed_table_->selectionModel()->selectedIndexes()) {
    const QModelIndex source = executed_proxy_.mapToSource(i);
    if (source.isValid()) rows.insert(static_cast<std::size_t>(source.row()));
  }
  return {rows.begin(), rows.end()};
}

bool ExperimentWindow::reset_executed(QString* error) {
  // The model refuses while live too: the queue may have ended without the
  // window having heard yet.
  if (bridge_.running() || model_.live() || !model_.reset_executed()) {
    if (error != nullptr) *error = tr("a queue is running");
    return false;
  }
  return true;
}

bool ExperimentWindow::clear_executed(QString* error) {
  auto set_error = [&](const QString& e) {
    if (error != nullptr) *error = e;
    return false;
  };
  if (bridge_.running() || model_.live()) return set_error(tr("a queue is running"));
  const std::size_t runs = model_.executed_rows();
  if (runs == 0) return set_error(tr("no run has been executed"));
  if (!ask_clear_executed_(static_cast<int>(runs))) return set_error(tr("cancelled"));
  // The question ran an event loop: a queue may have started under it.
  if (bridge_.running() || model_.live()) return set_error(tr("a queue is running"));
  if (!model_.clear_executed()) return set_error(tr("the queue cannot change"));
  return true;  // edited() marked the queue modified
}

bool ExperimentWindow::repeat_last(QString* error) {
  auto set_error = [&](const QString& e) {
    if (error != nullptr) *error = e;
    return false;
  };
  const std::size_t executed = model_.executed_rows();
  if (executed == 0) return set_error(tr("no run has been executed"));
  auto rows = selected_executed_rows();
  if (rows.empty()) rows.push_back(executed - 1);  // the newest
  const auto inserted = model_.repeat(std::move(rows));
  // Refused live: editRefused has shown the executor's reason in the pane.
  if (!inserted) return set_error(tr("the runs could not be repeated"));
  select_rows(*inserted);
  if (!inserted->empty()) {
    const QModelIndex at = queue_proxy_.mapFromSource(model_.index(static_cast<int>(inserted->front()), 0));
    if (at.isValid()) queue_table_->scrollTo(at);
  }
  return true;
}

void ExperimentWindow::inform_refusal(const QString& error) {
  if (error == tr("cancelled")) return;
  QString text = error;
  if (error == tr("a queue is running")) text = tr("A queue is running.");
  else if (error == tr("no run has been executed")) text = tr("No run has been executed.");
  else if (!text.isEmpty()) text = text.at(0).toUpper() + text.mid(1) + QLatin1Char('.');
  inform_(text);
}

bool ExperimentWindow::offer_resume(QString* error) {
  if (bridge_.running() || model_.live()) {
    if (error != nullptr) *error = tr("a queue is running");
    return false;
  }
  // The window that just stopped keeps its executed rows; Start goes on from
  // them (design 5: not offered right after a Stop).
  if (model_.executed_rows() > 0) return false;
  const std::filesystem::path file = bridge_.session().data() / "executor_state.json";
  auto state = experiment::executor::Executor::read_state(file);
  if (!state) return false;  // no file, or one that does not read: nothing to offer
  const QDateTime written = QFileInfo(QString::fromStdString(file.string())).lastModified();
  const auto offer = resume_offer(*state, model_.queue(), written);
  if (!offer) return false;
  if (!ask_resume_(*offer) || !offer->resumable()) return false;
  // The question ran an event loop: a queue may have started under it.
  if (bridge_.running() || model_.live()) {
    if (error != nullptr) *error = tr("a queue is running");
    return false;
  }
  if (!model_.resume_from(offer->next_row, offer->statuses)) return false;
  start();
  return true;
}

bool ExperimentWindow::ask_resume_dialog(const ResumeOffer& offer) {
  QMessageBox box(QMessageBox::Question, tr("Resume Queue"), offer.question, QMessageBox::NoButton, this);
  QString detail = offer.how;
  if (!offer.resumable()) detail += QLatin1Char('\n') + offer.mismatch;
  box.setInformativeText(detail);
  QPushButton* resume = box.addButton(tr("Resume"), QMessageBox::AcceptRole);
  QPushButton* not_now = box.addButton(tr("Not now"), QMessageBox::RejectRole);
  resume->setEnabled(offer.resumable());
  box.setDefaultButton(offer.resumable() ? resume : not_now);
  box.exec();
  return box.clickedButton() == resume;
}

ScriptEditorWindow* ExperimentWindow::open_script_editor() {
  if (script_editor_ == nullptr) {
    // Its own settings object on the same store as this window's.
    script_editor_ = new ScriptEditorWindow(
        bridge_.lab(), std::make_unique<QSettings>(settings_->fileName(), settings_->format()), this);
    script_editor_->setWindowFlag(Qt::Window);
    // A new or saved script may fix (or break) rows that name it.
    connect(script_editor_, &ScriptEditorWindow::scriptsChanged, this, [this] { model_.revalidate(); });
    connect(script_editor_, &ScriptEditorWindow::rehearseRequested, this, &ExperimentWindow::rehearseRequested);
  }
  script_editor_->show();
  script_editor_->raise();
  script_editor_->activateWindow();
  return script_editor_;
}

ConditionalsEditorWindow* ExperimentWindow::open_conditionals_editor(const QString& file) {
  if (conditionals_editor_ == nullptr) {
    // Its own settings object on the same store as this window's.
    conditionals_editor_ = new ConditionalsEditorWindow(
        bridge_.lab(), std::make_unique<QSettings>(settings_->fileName(), settings_->format()), this);
    conditionals_editor_->setWindowFlag(Qt::Window);
    // A saved, new or deleted file may fix (or break) the queue and rows that name it.
    auto changed = [this] {
      model_.revalidate();
      factory_->refresh_conditionals();
    };
    connect(conditionals_editor_, &ConditionalsEditorWindow::saved, this, changed);
    connect(conditionals_editor_, &ConditionalsEditorWindow::filesChanged, this, changed);
    conditionals_editor_->set_referenced([this](const QString& name) {
      const std::string n = name.toStdString();
      const auto& queue = model_.queue();
      if (queue.queue_conditionals == n) return true;
      return std::any_of(queue.runs.begin(), queue.runs.end(), [&](const experiment::RunSpec& r) {
        return std::any_of(r.conditionals.begin(), r.conditionals.end(), [&](const auto& c) { return c.name == n; });
      });
    });
  }
  if (!file.isEmpty()) conditionals_editor_->open(file);
  conditionals_editor_->show();
  conditionals_editor_->raise();
  conditionals_editor_->activateWindow();
  return conditionals_editor_;
}

void ExperimentWindow::sync_queue_conditionals() {
  const QString current = QString::fromStdString(model_.queue().queue_conditionals);
  const QSignalBlocker block(queue_conditionals_);
  queue_conditionals_->clear();
  queue_conditionals_->addItem(tr("(none)"), QString());
  QStringList names;
  if (auto files = bridge_.lab().condition_files->list())
    for (const auto& n : *files) names.append(QString::fromStdString(n));
  for (const QString& n : names) queue_conditionals_->addItem(n, n);
  if (!current.isEmpty() && !names.contains(current)) {
    // The queue names a file the lab does not have: shown, not dropped.
    queue_conditionals_->addItem(current, current);
    queue_conditionals_->setItemData(queue_conditionals_->count() - 1, QBrush(theme().error_text), Qt::ForegroundRole);
  }
  queue_conditionals_->setCurrentIndex(std::max(0, queue_conditionals_->findData(current)));
  // The queue-wide checks are loaded when the queue starts.
  queue_conditionals_->setEnabled(!model_.live() && !model_.locked());
}

bool ExperimentWindow::edit_selected_conditionals() {
  const auto rows = selected_rows();
  // The dialog runs an event loop; the executor may change the queue under it.
  const experiment::QueueSpec asked_about = model_.queue();
  const auto& runs = asked_about.runs;
  if (rows.empty() || rows.back() >= runs.size()) return false;
  auto has = [&](std::size_t row, const std::string& name) {
    const auto& c = runs[row].conditionals;
    return std::any_of(c.begin(), c.end(), [&](const auto& ref) { return ref.name == name; });
  };
  // The lab's files, then names the rows reference that the lab lacks.
  std::vector<std::string> names;
  if (auto files = bridge_.lab().condition_files->list()) names = *files;
  for (const std::size_t row : rows)
    for (const auto& c : runs[row].conditionals)
      if (std::find(names.begin(), names.end(), c.name) == names.end()) names.push_back(c.name);
  QStringList shown;
  QList<Qt::CheckState> states;
  for (const auto& name : names) {
    const auto n = static_cast<std::size_t>(
        std::count_if(rows.begin(), rows.end(), [&](std::size_t row) { return has(row, name); }));
    shown.append(QString::fromStdString(name));
    states.append(n == 0 ? Qt::Unchecked : n == rows.size() ? Qt::Checked : Qt::PartiallyChecked);
  }
  const auto wanted = pick_conditionals_(shown, states);
  if (!wanted || wanted->size() != states.size()) return false;
  if (!(model_.queue() == asked_about)) {
    pane_->show_error(tr("The queue changed while choosing; the conditionals were not applied"));
    return false;
  }

  // Per row: what it keeps, in its order, then what it gains, in list order.
  QueueTableModel::RowConditionals per_row;
  for (const std::size_t row : rows) {
    auto state_of = [&](const std::string& name) {
      const auto i = std::find(names.begin(), names.end(), name) - names.begin();
      return (*wanted)[static_cast<int>(i)];
    };
    std::vector<std::string> result;
    for (const auto& c : runs[row].conditionals)
      if (state_of(c.name) != Qt::Unchecked) result.push_back(c.name);
    for (const auto& name : names)
      if (state_of(name) == Qt::Checked && !has(row, name)) result.push_back(name);
    per_row.emplace_back(row, std::move(result));
  }
  const bool ok = model_.set_conditionals(per_row);  // one edit: all rows or none
  select_rows(rows);
  return ok;
}

std::optional<QList<Qt::CheckState>> ExperimentWindow::pick_conditionals_dialog(const QStringList& names,
                                                                                const QList<Qt::CheckState>& states) {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Conditionals for the selected runs"));
  auto* list = new QListWidget;
  for (int i = 0; i < names.size(); ++i) {
    auto* item = new QListWidgetItem(names[i], list);
    Qt::ItemFlags flags = item->flags() | Qt::ItemIsUserCheckable;
    // Only a file some rows have can stay "as each row has it".
    if (states[i] == Qt::PartiallyChecked) flags |= Qt::ItemIsUserTristate;
    item->setFlags(flags);
    item->setCheckState(states[i]);
  }
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  auto* edit = buttons->addButton(tr("Edit..."), QDialogButtonBox::ActionRole);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(edit, &QPushButton::clicked, &dialog, [&] {
    const QString file = list->currentItem() != nullptr ? list->currentItem()->text() : QString();
    dialog.reject();
    open_conditionals_editor(file);
  });
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(tr("A half-ticked file stays as each run has it.")));
  layout->addWidget(list);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) return std::nullopt;
  QList<Qt::CheckState> out;
  for (int i = 0; i < list->count(); ++i) out.append(list->item(i)->checkState());
  return out;
}

bool ExperimentWindow::edit_row_script(scripting::ScriptKind kind) {
  const auto rows = selected_rows();
  return rows.size() == 1 && open_row_script(rows.front(), kind);
}

bool ExperimentWindow::open_row_script(std::size_t row, scripting::ScriptKind kind) {
  if (row >= model_.queue().runs.size()) return false;
  const auto& run = model_.queue().runs[row];
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
  const std::string spectrometer = model_.queue().mass_spectrometer;
  model_.set_queue(std::move(*q));
  references_follow(spectrometer);
  path_ = path;
  set_modified(false);
  settings_->setValue(QStringLiteral("experiment_window/last_queue"), QString::fromStdString(path.string()));
  recent_.add(path);
  rebuild_recent();
  return true;
}

bool ExperimentWindow::new_queue(QString* error) {
  auto set_error = [&](const QString& e) {
    if (error) *error = e;
    return false;
  };
  if (bridge_.running()) return set_error(tr("a queue is running"));
  if (!resolve_unsaved()) return set_error(tr("cancelled"));
  const std::string spectrometer = model_.queue().mass_spectrometer;
  model_.set_queue({});
  references_follow(spectrometer);
  path_.reset();
  set_modified(false);
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
  recent_.add(path);
  rebuild_recent();
  return true;
}

void ExperimentWindow::new_dialog() {
  if (bridge_.running()) {
    QMessageBox::information(this, tr("New"), tr("A queue is running."));
    return;
  }
  new_queue();
}

void ExperimentWindow::open_dialog() {
  if (bridge_.running()) {
    QMessageBox::information(this, tr("Open"), tr("A queue is running."));
    return;
  }
  // The directory of the last queue opened while it is still there, else
  // the lab's queues/ (queue-directory design, section 3).
  std::filesystem::path start = bridge_.lab().paths.queues();
  const QString last = settings_->value(QStringLiteral("experiment_window/last_queue")).toString();
  if (!last.isEmpty()) {
    const std::filesystem::path dir = std::filesystem::path(last.toStdString()).parent_path();
    std::error_code ec;
    if (!dir.empty() && std::filesystem::is_directory(dir, ec)) start = dir;
  }
  const QString file = pick_open_file_(QString::fromStdString(start.string()));
  if (file.isEmpty()) return;
  QString error;
  if (!load_queue(file.toStdString(), &error)) {
    if (error != tr("cancelled")) QMessageBox::warning(this, tr("Open"), error);
    return;
  }
  offer_resume();
}

void ExperimentWindow::save_as_dialog() {
  // A queue with a file starts at that file; a new one in the lab's queues/,
  // made first when missing (the dialog still opens, at the lab, when it
  // cannot be made).
  QString start;
  if (path_) {
    start = QString::fromStdString(path_->string());
  } else {
    const auto& paths = bridge_.lab().paths;
    std::error_code ec;
    std::filesystem::create_directories(paths.queues(), ec);
    const bool made = std::filesystem::is_directory(paths.queues(), ec);
    start = QString::fromStdString((made ? paths.queues() : paths.dir).string());
  }
  const QString file = pick_save_file_(start);
  if (file.isEmpty()) return;
  QString error;
  if (!save_as(file.toStdString(), &error)) QMessageBox::warning(this, tr("Save"), error);
}

void ExperimentWindow::start() {
  if (bridge_.running()) return;
  if (peak_center_running_ && peak_center_running_()) {
    pane_->show_error(tr("peak center running in the spectrometer window"));
    return;
  }
  // The first row of the Queue table (queue-tables design 3): the executed
  // rows keep their statuses.
  const std::size_t from = model_.executed_rows();
  auto r = bridge_.start(model_.queue(), from);
  if (!r) {
    pane_->show_error(QString::fromStdString(r.error().what));
    return;
  }
  model_.clear_status(from);
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
  sync_queue_conditionals();
}

void ExperimentWindow::set_executed_collapsed(bool collapsed) {
  const bool changed = collapsed != executed_collapsed_;
  executed_collapsed_ = collapsed;
  executed_table_->setVisible(!collapsed);
  executed_fold_->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
  executed_fold_->setToolTip(collapsed ? tr("Show the executed runs") : tr("Hide the executed runs"));
  // Collapsed, the splitter gives the section no more than its header.
  executed_section_->setMaximumHeight(collapsed ? executed_header_->sizeHint().height() : QWIDGETSIZE_MAX);
  if (changed) settings_->setValue(QStringLiteral("experiment_window/executed_collapsed"), collapsed);
}

void ExperimentWindow::on_executed_changed() {
  const auto rows = model_.executed_rows();
  executed_count_->setText(tr("Executed (%1)").arg(static_cast<qulonglong>(rows)));
  executed_section_->setVisible(rows > 0);
  if (executed_follow_) executed_table_->scrollToBottom();
  update_state();
}

void ExperimentWindow::closeEvent(QCloseEvent* event) {
  if (!resolve_unsaved() || (script_editor_ != nullptr && script_editor_->isVisible() && !script_editor_->close()) ||
      (conditionals_editor_ != nullptr && conditionals_editor_->isVisible() && !conditionals_editor_->close())) {
    event->ignore();
    return;
  }
  if (bridge_.running() && ask_stop_()) bridge_.stop();
  layouts_->save_last();
  settings_->setValue(QStringLiteral("experiment_window/tables_splitter"), tables_splitter_->saveState());
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
