#include "flux_window.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "brand.hpp"
#include "flux_analysis_model.hpp"
#include "flux_monitor_model.hpp"
#include "flux_unknown_model.hpp"
#include "options_editor.hpp"
#include "preset_bar.hpp"
#include "pychron/core/user_file.hpp"
#include "pychron/processing/flux_view.hpp"
#include "scene_view.hpp"
#include "theme.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/flux_store.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

namespace {

namespace pp = pychron::processing;
namespace ps = pychron::persistence;

constexpr int kFitDelayMs = 150;  // W5: a refit follows the last change by this much

// The tree as the store has it: read in one job, drawn on the GUI thread.
struct TreeLevel {
  std::string name;
  pp::LevelFluxStatus status = pp::LevelFluxStatus::NoMonitors;
};
struct TreeIrradiation {
  std::string name;
  std::vector<TreeLevel> levels;
};
struct Tree {
  std::vector<TreeIrradiation> irradiations;
  pp::MonitorSets sets;  // of the store's document, for the monitor group
};
// A level, with the monitor sets it was chosen among.
struct LoadedLevel {
  pp::LevelInputs inputs;
  pp::MonitorSets sets;
};
// What a save came to; of a conflict, who moved the head and when (read in
// the same job, on the worker).
struct SaveResult {
  pp::FluxSaveOutcome outcome;
  std::string saved_by, saved_utc;
};

Result<Tree> read_tree(ps::IStore& store) {
  auto sets = pp::load_monitor_sets(store);
  if (!sets) return fail(sets.error());
  const pp::MonitorSet* standard = sets->sets.find("");
  const std::string monitor_sample = standard ? standard->sample : std::string();

  auto irradiations = store.irradiations();
  if (!irradiations) return fail(irradiations.error());
  Tree tree;
  tree.sets = std::move(sets->sets);
  auto& out = tree.irradiations;
  for (const auto& irradiation : *irradiations) {
    TreeIrradiation item{irradiation.name, {}};
    auto levels = store.levels(irradiation.uuid);
    if (!levels) return fail(levels.error());
    for (const auto& level : *levels) {
      auto sheet = store.level_sheet(level.uuid);
      if (!sheet) return fail(sheet.error());
      item.levels.push_back(
          {level.name, *sheet ? pp::level_flux_status(**sheet, monitor_sample) : pp::LevelFluxStatus::NoMonitors});
    }
    std::sort(item.levels.begin(), item.levels.end(),
              [](const TreeLevel& a, const TreeLevel& b) { return a.name < b.name; });
    out.push_back(std::move(item));
  }
  // Newest first: irradiations are named in the order they were made.
  std::sort(out.begin(), out.end(), [](const TreeIrradiation& a, const TreeIrradiation& b) { return a.name > b.name; });
  return tree;
}

bool no_edits(const pp::Edits& e) {
  return e.omit.empty() && e.include.empty() && e.exclude_positions.empty() && e.include_positions.empty() &&
         !e.reset_omits;
}

// The same fit options, whether a value was set or is its field's default
// (Options' own == tells those apart).
bool same_values(const pp::Options& a, const pp::Options& b) {
  for (const auto& field : pp::flux_options_schema()->fields)
    if (a.get(field.key) != b.get(field.key)) return false;
  return true;
}

const pp::LevelPosition* monitor_at(const pp::LevelInputs& inputs, int hole) {
  for (const auto& p : inputs.positions)
    if (p.hole == hole) return p.monitor ? &p : nullptr;
  return nullptr;
}

// A table's own notes on its column widths (size_columns).
constexpr const char* kSized = "flux_columns_sized";
constexpr const char* kSizing = "flux_columns_sizing";
constexpr const char* kDragged = "flux_columns_dragged";

QTableView* make_table(QAbstractItemModel* model, const QString& name, QWidget* parent) {
  auto* table = new QTableView(parent);
  table->setObjectName(name);
  table->setModel(model);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->verticalHeader()->setVisible(false);
  // The columns are the user's to drag; size_columns() gives them their first
  // widths. At 1400 pixels the monitor table's sixteen columns scroll rather
  // than clip.
  QHeaderView* header = table->horizontalHeader();
  header->setSectionResizeMode(QHeaderView::Interactive);
  header->setStretchLastSection(false);
  QObject::connect(header, &QHeaderView::sectionResized, table, [table] {
    if (!table->property(kSizing).toBool()) table->setProperty(kDragged, true);
  });
  return table;
}

// Every column as wide as its head and its widest cell, once: when the table
// first has rows, and again when it first has all it shows (`complete`: a fit's
// predictions) or another number of columns. Never over a width the user
// dragged, so a refit leaves the columns alone.
void size_columns(QTableView* table, bool complete) {
  const int columns = table->model()->columnCount();
  if (table->model()->rowCount() == 0) return;
  const int want = columns * 2 + (complete ? 1 : 0);
  const int have = table->property(kSized).toInt();
  if (have / 2 == columns && (have >= want || table->property(kDragged).toBool())) return;
  table->setProperty(kSizing, true);
  table->resizeColumnsToContents();
  table->setProperty(kSizing, false);
  table->setProperty(kSized, want);
  if (have / 2 != columns) table->setProperty(kDragged, false);
}

}  // namespace

FluxWindow::FluxWindow(EntryBridge& bridge, pp::IAnalysisSource& source, pp::PresetStore& presets, QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      source_(source),
      presets_(presets),
      monitors_(new FluxMonitorModel(this)),
      unknowns_(new FluxUnknownModel(this)),
      analyses_(new FluxAnalysisModel(this)),
      values_(presets.defaults(pp::flux_options_schema())),
      loaded_values_(values_),
      preset_name_(QStringLiteral("Default")) {
  setObjectName(QStringLiteral("flux_window"));
  resize(1400, 820);
  ask_unsaved_ = [this](const QString& question) {
    const auto answer = QMessageBox::question(this, windowTitle(), question,
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    return answer == QMessageBox::Save ? Unsaved::Save : answer == QMessageBox::Discard ? Unsaved::Discard : Unsaved::Cancel;
  };

  auto* split = new QSplitter(Qt::Horizontal, this);
  tree_ = new QTreeWidget(split);
  tree_->setObjectName(QStringLiteral("flux_tree"));
  tree_->setHeaderLabels({tr("Level"), tr("Flux")});
  tree_->setMinimumWidth(200);

  auto* center = new QSplitter(Qt::Vertical, split);
  view_ = new SceneView(center);
  auto* tables = new QSplitter(Qt::Horizontal, center);
  auto* monitor_side = new QSplitter(Qt::Vertical, tables);
  monitor_table_ = make_table(monitors_, QStringLiteral("flux_monitors"), monitor_side);
  analysis_table_ = make_table(analyses_, QStringLiteral("flux_analyses"), monitor_side);
  monitor_side->setStretchFactor(0, 2);
  monitor_side->setStretchFactor(1, 1);
  unknown_table_ = make_table(unknowns_, QStringLiteral("flux_unknowns"), tables);
  tables->setStretchFactor(0, 3);
  tables->setStretchFactor(1, 2);
  center->setStretchFactor(0, 3);
  center->setStretchFactor(1, 2);
  split->setStretchFactor(1, 1);
  // The size hints of the tables would take the plot's height and the centre's
  // width at first show: say how the first 1400 x 820 is shared (the dock takes
  // its own width off the right).
  split->setSizes({200, 840});
  center->setSizes({380, 360});
  tables->setSizes({500, 340});
  monitor_side->setSizes({250, 110});
  setCentralWidget(split);

  auto* bar = addToolBar(tr("Flux"));
  bar->setObjectName(QStringLiteral("flux_toolbar"));
  save_action_ = bar->addAction(tr("Save"), this, [this] { save(); });
  revert_action_ = bar->addAction(tr("Revert"), this, [this] { revert(); });
  revert_action_->setToolTip(tr("Drop the edits and return to the options the level was loaded with"));
  reload_action_ = bar->addAction(tr("Reload"), this, [this] { reload(); });
  reload_action_->setToolTip(tr("Read the level again"));
  reset_action_ = bar->addAction(tr("Reset omissions"), this, [this] { reset_omissions(); });
  reset_action_->setToolTip(tr("Forget what the saved fit omitted and excluded; tags still apply"));
  export_action_ = bar->addAction(tr("Export CSV…"), this, [this] { export_asked(); });
  export_action_->setToolTip(tr("Write the fitted positions to a CSV file"));
  packages_action_ = bar->addAction(tr("Open in Packages"), this, [this] {
    if (inputs_) Q_EMIT packages_requested(irradiation_, level_);
  });
  packages_action_->setToolTip(tr("Show this level in the Packages window"));

  build_dock();

  status_ = new QLabel;
  status_->setObjectName(QStringLiteral("flux_status"));
  status_->setWordWrap(true);
  statusBar()->addWidget(status_, 1);

  fit_timer_ = new QTimer(this);
  fit_timer_->setSingleShot(true);
  fit_timer_->setInterval(kFitDelayMs);
  connect(fit_timer_, &QTimer::timeout, this, [this] { fit_now(); });

  connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
    if (!item || !item->parent()) return;  // an irradiation is not a level
    open_level(item->parent()->text(0), item->text(0));
  });
  connect(monitor_table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex& current) {
            if (resetting_) return;
            selected_hole_ = current.isValid() ? std::optional<int>(monitors_->hole_at(current.row())) : std::nullopt;
            show_selected();
            update_scene();
          });
  // W9: a plot click, a rubber band and a check box are one edit.
  connect(view_, &SceneView::point_clicked, this, [this](const QString& uuid) { toggle_analyses({uuid}); });
  connect(view_, &SceneView::points_toggled, this, [this](const QStringList& uuids) { toggle_analyses(uuids); });
  connect(view_, &SceneView::recall_requested, this, [this](const QString& uuid) {
    if (open_recall_) open_recall_(uuid);
  });
  connect(analyses_, &FluxAnalysisModel::use_toggled, this, [this](const QString& uuid, bool use) {
    if (!set_used(uuid.toStdString(), use)) return;
    forget_message();
    request_fit();
    refresh_status();
  });
  connect(monitors_, &FluxMonitorModel::fit_toggled, this, [this](int hole, bool in_fit) { set_in_fit(hole, in_fit); });
  connect(monitors_, &FluxMonitorModel::save_toggled, this, [this](int hole, bool save) { set_save(hole, save); });
  connect(unknowns_, &FluxUnknownModel::save_toggled, this, [this](int hole, bool save) { set_save(hole, save); });
  // Someone else's change (after its own save the window reads the level
  // again itself): the tree always; the level only when no edit would be lost
  // with it.
  connect(&bridge_, &EntryBridge::changed, this, [this] {
    if (notifying_) return;
    reload_tree();
    if (irradiation_.isEmpty()) return;
    // While a save runs the level is not read again under it: the save reads
    // it when it wrote, and else the status says there is something to see.
    if (!saving_ && !edited()) return start_load();
    changed_elsewhere_ = true;
    refresh_status();
  });

  update_title();
  update_actions();
  reload_tree();
}

FluxWindow::~FluxWindow() {
  // The models are children, destroyed after the members they point into.
  resetting_ = true;
  analyses_->set_position(nullptr);
  for (FluxPositionModel* model : {static_cast<FluxPositionModel*>(monitors_), static_cast<FluxPositionModel*>(unknowns_)}) {
    model->set_fit(nullptr);
    model->set_unfitted(nullptr);
    model->set_inputs(nullptr);
  }
}

void FluxWindow::build_dock() {
  auto* dock = new QDockWidget(tr("Fit"), this);
  dock->setObjectName(QStringLiteral("flux_dock"));
  auto* host = new QWidget;
  auto* layout = new QVBoxLayout(host);

  // W7: the monitor selection belongs to the level, not to a preset.
  auto* group = new QGroupBox(tr("Monitors"));
  group->setObjectName(QStringLiteral("flux_monitor_group"));
  auto* form = new QFormLayout(group);
  set_combo_ = new QComboBox;
  form->addRow(tr("Monitor set"), set_combo_);
  sample_edit_ = new QLineEdit;
  sample_edit_->setToolTip(tr("The sample whose positions are the monitors; empty: the monitor set's own"));
  form->addRow(tr("Sample"), sample_edit_);
  all_box_ = new QCheckBox(tr("All positions"));
  all_box_->setToolTip(tr("Every position that has analyses is a monitor"));
  form->addRow(all_box_);
  layout->addWidget(group);
  connect(set_combo_, &QComboBox::currentIndexChanged, this, [this] { group_changed(); });
  connect(sample_edit_, &QLineEdit::editingFinished, this, [this] { group_changed(); });
  connect(all_box_, &QCheckBox::toggled, this, [this] { group_changed(); });

  preset_bar_ = new PresetBar(presets_, pp::flux_options_schema());
  // What the editor shows, also when it is no fit's: options_ would then be older.
  preset_bar_->current = [this] { return values_; };
  layout->addWidget(preset_bar_);
  editor_ = new OptionsEditor;
  layout->addWidget(editor_, 1);
  dock->setWidget(host);
  dock_host_ = host;
  addDockWidget(Qt::RightDockWidgetArea, dock);

  editor_->set_options(values_);
  preset_bar_->reload(preset_name_);
  connect(preset_bar_, &PresetBar::loaded, this, [this](const pp::Options& options, const QString& name) {
    preset_name_ = name;
    set_options(options);
  });
  connect(preset_bar_, &PresetBar::pinned_chosen, this, [this] { set_options(loaded_values_); });
  // After the status, not over it: a fit error and "edited" stay in sight.
  connect(preset_bar_, &PresetBar::message, this, [this](const QString& text, const QString& details) {
    message_ = text;
    message_details_ = details;
    update_tooltip();
    refresh_status();
  });
  connect(editor_, &OptionsEditor::changed, this, [this] {
    values_ = editor_->options();
    options_changed();
  });
}

bool FluxWindow::busy() const noexcept { return busy_ > 0 || fit_timer_->isActive(); }

QString FluxWindow::status() const { return status_->text(); }

void FluxWindow::set_status(const QString& text, bool error, const QStringList& warnings) {
  warnings_ = warnings;
  status_error_ = error;
  status_text_ = warnings.isEmpty()       ? text
                 : warnings.size() == 1 ? tr("%1 · 1 warning").arg(text)
                                        : tr("%1 · %2 warnings").arg(text).arg(warnings.size());
  update_tooltip();
  style::set_tone(status_, error ? style::Tone::Error : style::Tone::Normal);
  refresh_status();
}

void FluxWindow::say(const QString& text, bool error) {
  status_error_ = error;
  status_text_ = text;
  style::set_tone(status_, error ? style::Tone::Error : style::Tone::Normal);
  refresh_status();
}

void FluxWindow::update_tooltip() {
  QStringList lines = warnings_;
  if (!tree_error_.isEmpty()) lines << tree_error_;
  if (!message_details_.isEmpty()) lines << message_details_;
  status_->setToolTip(lines.join(QLatin1Char('\n')));
}

void FluxWindow::refresh_status() {
  QString text = status_text_;
  if (!message_.isEmpty()) text = text.isEmpty() ? message_ : tr("%1 · %2").arg(text, message_);
  if (inputs_ && changed_elsewhere_) text = tr("%1 · level changed elsewhere, Reload to see it").arg(text);
  if (edited()) text = tr("%1 · edited (not saved)").arg(text);
  status_->setText(text);
}

void FluxWindow::forget_message() {
  if (message_.isEmpty() && message_details_.isEmpty()) return;
  message_.clear();
  message_details_.clear();
  update_tooltip();
  refresh_status();
}

void FluxWindow::update_actions() {
  const bool idle = !loading_ && !saving_;
  reload_action_->setEnabled(idle && !irradiation_.isEmpty());
  revert_action_->setEnabled(idle && inputs_.has_value());
  reset_action_->setEnabled(idle && inputs_.has_value());
  export_action_->setEnabled(idle && fit_.has_value());
  packages_action_->setEnabled(idle && inputs_.has_value());
  // Section 5.5: when it cannot save, Save says why.
  save_action_->setEnabled(idle && fit_.has_value());
  save_action_->setToolTip(saving_    ? tr("Saving…")
                           : loading_ ? tr("Loading…")
                           : !inputs_ ? tr("No level open")
                           : !fit_    ? (fit_error_.empty() ? tr("Fitting…") : QString::fromStdString(fit_error_))
                                      : tr("Save the predicted J of every position whose Save box is ticked"));
}

void FluxWindow::restore_selection() {
  const int row = selected_hole_ ? monitors_->row_of(*selected_hole_) : -1;
  if (row >= 0)
    monitor_table_->setCurrentIndex(monitors_->index(row, 0));
  else
    selected_hole_.reset();
}

void FluxWindow::update_title() {
  setWindowTitle(inputs_ ? tr("Flux — %1 %2").arg(irradiation_, level_) : tr("Flux"));
}

void FluxWindow::update_enabled() {
  for (QTableView* table : {monitor_table_, analysis_table_, unknown_table_}) table->setEnabled(!loading_ && !saving_);
  // What is being saved is not edited meanwhile: the reload would drop it unsaid.
  view_->setEnabled(!saving_);
  dock_host_->setEnabled(!saving_);
}

// ---- The tree ---------------------------------------------------------------

QTreeWidgetItem* FluxWindow::tree_item(const QString& irradiation, const QString& level) const {
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* top = tree_->topLevelItem(i);
    if (top->text(0) != irradiation) continue;
    for (int c = 0; c < top->childCount(); ++c)
      if (top->child(c)->text(0) == level) return top->child(c);
  }
  return nullptr;
}

void FluxWindow::select_tree_item() {
  QTreeWidgetItem* item = tree_item(irradiation_, level_);
  if (!item || (item == tree_->currentItem() && item->isSelected())) return;
  const QSignalBlocker blocked(tree_);
  item->parent()->setExpanded(true);
  tree_->setCurrentItem(item);
}

void FluxWindow::reload_tree() {
  const quint64 generation = ++tree_generation_;
  ++busy_;
  ++tree_jobs_;
  bridge_.run<Tree>(
      this, [](ps::IStore& store, const ps::Actor&) { return read_tree(store); },
      [this, generation](Result<Tree> tree) {
        --busy_;
        --tree_jobs_;
        if (generation != tree_generation_) return;  // a newer read is on its way
        if (!tree) {
          // Said in the status only when no level's own would be lost for it.
          const QString error = QString::fromStdString(tree.error().what);
          if (!inputs_ && !loading_ && !saving_) return set_status(error, true);
          tree_error_ = error;
          return update_tooltip();
        }
        tree_error_.clear();
        update_tooltip();
        set_monitor_sets(std::move(tree->sets.sets), std::move(tree->sets.default_name));
        const auto* read = &tree->irradiations;
        {
          const QSignalBlocker blocked(tree_);
          QStringList open;  // the irradiations the user had unfolded stay so
          for (int i = 0; i < tree_->topLevelItemCount(); ++i)
            if (tree_->topLevelItem(i)->isExpanded()) open << tree_->topLevelItem(i)->text(0);
          const bool first = tree_->topLevelItemCount() == 0;
          tree_->clear();
          for (const auto& irradiation : *read) {
            auto* top = new QTreeWidgetItem(tree_);
            top->setText(0, QString::fromStdString(irradiation.name));
            top->setExpanded(open.contains(top->text(0)));
            for (const auto& level : irradiation.levels) {
              auto* item = new QTreeWidgetItem(top);
              item->setText(0, QString::fromStdString(level.name));
              item->setText(1, level.status == pp::LevelFluxStatus::Fitted      ? tr("fitted")
                               : level.status == pp::LevelFluxStatus::NotFitted ? tr("not fitted")
                                                                                : tr("no monitors"));
              item->setForeground(1, theme().muted_text);
            }
          }
          // The first time, the newest irradiation's levels are in view.
          if (first && tree_->topLevelItemCount() > 0) tree_->topLevelItem(0)->setExpanded(true);
          tree_->resizeColumnToContents(0);
        }
        select_tree_item();
      });
}

// ---- Loading ----------------------------------------------------------------

void FluxWindow::open_level(const QString& irradiation, const QString& level) {
  leave(
      true,
      [this, irradiation, level] {
        irradiation_ = irradiation;
        level_ = level;
        chosen_.reset();  // another level: its monitors are as its saved fit chose them
        baseline_.reset();
        selected_hole_.reset();
        select_tree_item();
        if (tree_->topLevelItemCount() == 0 && tree_jobs_ == 0) reload_tree();
        start_load();
      },
      [this] {
        // Back on the level that stays; again once the click that asked is
        // over, which selects the row it landed on after this returns.
        select_tree_item();
        QTimer::singleShot(0, this, [this] { select_tree_item(); });
      });
}

void FluxWindow::reload() {
  if (irradiation_.isEmpty()) return;
  leave(true, [this] { start_load(); }, {});
}

void FluxWindow::clear_level() {
  fit_timer_->stop();
  resetting_ = true;
  analyses_->set_position(nullptr);
  for (FluxPositionModel* model : {static_cast<FluxPositionModel*>(monitors_), static_cast<FluxPositionModel*>(unknowns_)}) {
    model->set_fit(nullptr);
    model->set_unfitted(nullptr);
    model->set_inputs(nullptr);
  }
  evaluated_.clear();
  skip_.clear();
  loaded_skip_.clear();
  monitors_->set_skip(skip_);
  unknowns_->set_skip(skip_);
  resetting_ = false;
  fit_.reset();
  unfitted_.reset();
  inputs_.reset();
  fit_error_.clear();
  selected_hole_.reset();
  edits_ = {};
  options_ = {};  // no level's: not the last one's
  view_->set_scene(nullptr);
  update_title();
}

void FluxWindow::start_load() {
  const quint64 generation = ++load_generation_;
  ++loads_started_;
  reselect_ = selected_hole_;  // read again, the level keeps its selected monitor
  forget_message();
  clear_level();
  loading_ = true;
  changed_elsewhere_ = false;
  update_enabled();
  update_actions();
  set_status(tr("Loading %1 %2…").arg(irradiation_, level_), false);
  ++busy_;
  // What is not chosen here is as the level's saved fit had it.
  pp::MonitorSelection selection;
  if (chosen_) {
    selection.monitor_set = chosen_->set;
    if (!chosen_->sample.empty()) selection.sample = chosen_->sample;
    selection.all_positions = chosen_->all_positions;
  }
  // The job runs on the worker and may outlive the window: it takes values and
  // the source, never `this`.
  bridge_.run<LoadedLevel>(
      this,
      [source = &source_, irradiation = irradiation_.toStdString(), level = level_.toStdString(), selection](
          ps::IStore& store, const ps::Actor&) -> Result<LoadedLevel> {
        auto sets = pp::load_monitor_sets(store);
        if (!sets) return fail(sets.error());
        auto inputs = pp::load_level(*source, store, irradiation, level, selection);
        if (!inputs) return fail(inputs.error());
        return LoadedLevel{std::move(*inputs), std::move(sets->sets)};
      },
      [this, generation](Result<LoadedLevel> loaded) {
        --busy_;
        if (generation != load_generation_) return;  // superseded by a newer selection
        loading_ = false;
        update_enabled();
        update_actions();
        if (!loaded) {
          // What the save that asked for this read said is not lost with it.
          QString error = QString::fromStdString(loaded.error().what);
          if (!note_.isEmpty() && note_generation_ == load_generation_) error = tr("%1 · %2").arg(note_, error);
          note_.clear();
          return set_status(error, true);
        }
        apply_loaded(std::move(loaded->inputs), std::move(loaded->sets.sets), std::move(loaded->sets.default_name));
      });
}

void FluxWindow::apply_loaded(pp::LevelInputs inputs, std::vector<pp::MonitorSet> sets, std::string default_set) {
  clear_level();
  inputs_ = std::move(inputs);

  // W6: the saved fit's options, else the preset in use.
  if (!preset_bar_->pinned_selected() && !preset_bar_->current_name().isEmpty())
    preset_name_ = preset_bar_->current_name();
  if (inputs_->saved_options) {
    loaded_preset_.clear();
    values_ = pp::to_options(*inputs_->saved_options);
  } else {
    loaded_preset_ = preset_name_;
    auto preset = presets_.load(pp::flux_options_schema(), preset_name_.toStdString());
    values_ = preset ? std::move(preset->options) : presets_.defaults(pp::flux_options_schema());
  }
  loaded_values_ = values_;
  options_ = {};
  editor_->set_options(values_);
  show_preset();
  resolve_options();

  set_monitor_sets(std::move(sets), std::move(default_set));
  show_group({inputs_->monitor_set.name, inputs_->monitor_set.sample, inputs_->all_positions});
  if (!chosen_) baseline_ = shown_;

  selected_hole_ = reselect_;
  resetting_ = true;
  monitors_->set_inputs(&*inputs_);
  unknowns_->set_inputs(&*inputs_);
  resetting_ = false;
  // R11: read again after its own save, the level keeps the Save boxes the
  // user unticked, and they are no edit: nothing differs from what was saved.
  if (kept_skip_generation_ == load_generation_ && !kept_skip_.empty()) {
    skip_ = kept_skip_;
    loaded_skip_ = skip_;
    apply_skip();
  }
  kept_skip_.clear();
  update_title();
  update_actions();
  fit_now();  // nothing to wait for: the debounce is for edits
}

void FluxWindow::show_preset() {
  preset_bar_->reload(loaded_preset_.isEmpty() ? preset_name_ : loaded_preset_);
  if (inputs_ && inputs_->saved_options) preset_bar_->set_pinned_item(tr("(saved fit)"));
}

// ---- Fitting ----------------------------------------------------------------

void FluxWindow::request_fit() {
  if (inputs_) fit_timer_->start();
}

void FluxWindow::fit_now() {
  fit_timer_->stop();
  if (!inputs_) return;
  resetting_ = true;
  analyses_->set_position(nullptr);
  for (FluxPositionModel* model : {static_cast<FluxPositionModel*>(monitors_), static_cast<FluxPositionModel*>(unknowns_)}) {
    model->set_fit(nullptr);
    model->set_unfitted(nullptr);
  }
  fit_.reset();
  unfitted_.reset();
  evaluated_.clear();
  fit_error_.clear();

  if (!options_error_.empty()) {
    fit_error_ = options_error_;  // options that are no fit's: nothing to run
  } else if (auto fitted = pp::fit_level(*inputs_, options_, edits_)) {
    fit_ = std::move(*fitted);
    monitors_->set_fit(&*fit_);
    unknowns_->set_fit(&*fit_);
  } else {
    fit_error_ = fitted.error().what;
  }
  if (!fit_) {
    // No fit: the positions as fit_level counts them, so the tables say which
    // monitors the edits leave in and one can be ticked back (ruling R15).
    evaluated_.reserve(inputs_->positions.size());
    for (const auto& position : inputs_->positions)
      evaluated_.push_back(pp::evaluate_position(position, inputs_->monitor_set, options_, edits_));
    monitors_->set_unfitted(&evaluated_);
    unknowns_->set_unfitted(&evaluated_);
  }

  // The selection is by hole: it survives the reset of the model.
  restore_selection();
  resetting_ = false;
  size_columns(monitor_table_, fit_.has_value());
  size_columns(unknown_table_, fit_.has_value());

  show_selected();
  update_scene();
  if (fit_) {
    // A monitor set chosen here replaces the saved fit's: no "missing" line for that one.
    const pp::FluxWarningContext context{chosen_.has_value(), false};
    QStringList warnings;
    for (const auto& line : pp::flux_warnings(*inputs_, *fit_, context)) warnings << QString::fromStdString(line);
    set_status(QString::fromStdString(pp::flux_status_line(*fit_)), false, warnings);
  } else {
    set_status(QString::fromStdString(fit_error_), true);
  }
  // What the save that read this level again said, in place of the fit's line.
  if (!note_.isEmpty()) {
    if ((fit_ || note_error_) && note_generation_ == load_generation_) say(note_, note_error_);
    note_.clear();
  }
  update_actions();
}

// ---- The selected monitor ---------------------------------------------------

void FluxWindow::select_monitor(int hole) {
  const int row = monitors_->row_of(hole);
  if (row < 0) return;
  monitor_table_->setCurrentIndex(monitors_->index(row, 0));
}

void FluxWindow::show_selected() {
  analyses_->set_position(nullptr);
  unfitted_.reset();
  if (!inputs_ || !selected_hole_) return;
  if (fit_) {
    const auto found = std::find_if(fit_->positions.begin(), fit_->positions.end(),
                                    [&](const pp::FittedPosition& p) { return p.hole == *selected_hole_; });
    if (found != fit_->positions.end() && found->monitor) analyses_->set_position(&*found);
    size_columns(analysis_table_, true);
    return;
  }
  // No fit: the analyses as fit_level counts them, so what is wrong can be seen.
  const auto found = std::find_if(inputs_->positions.begin(), inputs_->positions.end(),
                                  [&](const pp::LevelPosition& p) { return p.hole == *selected_hole_; });
  if (found == inputs_->positions.end() || !found->monitor) return;
  unfitted_ = pp::evaluate_position(*found, inputs_->monitor_set, options_, edits_);
  analyses_->set_position(&*unfitted_);
  size_columns(analysis_table_, true);
}

void FluxWindow::update_scene() {
  if (!inputs_) return view_->set_scene(nullptr);
  if (fit_) return view_->set_scene(pp::flux_scene(*inputs_, *fit_, pp::FluxSceneOptions{selected_hole_}));
  view_->set_scene(pp::flux_scene(*inputs_, options_, edits_));
}

// ---- Options ----------------------------------------------------------------

void FluxWindow::set_options(const pp::Options& options) {
  values_ = options;
  editor_->set_options(values_);
  options_changed();
}

void FluxWindow::resolve_options() {
  if (auto options = pp::flux_options_from(values_)) {
    options_ = *options;
    options_error_.clear();
  } else {
    options_error_ = options.error().what;
  }
  // The one thing the schema lets through that is no fit's is the error kind
  // (sd with a fitted surface): it is said on that field.
  if (QWidget* field = editor_->editor(QStringLiteral("fit.error"))) {
    style::set_invalid(field, !options_error_.empty());
    field->setToolTip(options_error_.empty() ? field->property("help").toString()
                                             : QString::fromStdString(options_error_));
  }
}

void FluxWindow::options_changed() {
  forget_message();
  resolve_options();
  if (!inputs_) return;
  if (options_error_.empty())
    request_fit();
  else
    fit_now();  // shows the error; there is nothing to fit
  refresh_status();
}

// ---- Edits (W9) -------------------------------------------------------------

bool FluxWindow::pending(bool with_group) const {
  if (!inputs_) return false;
  return !no_edits(edits_) || skip_ != loaded_skip_ || !same_values(values_, loaded_values_) ||
         (with_group && (group_edited() || !(group_shown() == shown_)));
}

bool FluxWindow::group_edited() const {
  // A choice made when the level could not be read without one is one too.
  return chosen_ && (!baseline_ || !(shown_ == *baseline_));
}

bool FluxWindow::edited() const noexcept { return pending(true); }

void FluxWindow::toggle_analyses(const QStringList& uuids) {
  if (!inputs_) return;
  bool changed = false;
  QSet<QString> seen;  // named twice, an analysis would be toggled back
  for (const QString& id : uuids) {
    if (seen.contains(id)) continue;
    seen.insert(id);
    if (set_used(id.toStdString(), std::nullopt)) changed = true;
  }
  if (!changed) return;
  forget_message();
  request_fit();
  refresh_status();
}

bool FluxWindow::set_used(const std::string& uuid, std::optional<bool> use) {
  if (!inputs_) return false;
  const pp::LevelPosition* position = nullptr;
  const pp::LevelAnalysis* analysis = nullptr;
  for (const auto& p : inputs_->positions) {
    if (!p.monitor) continue;
    for (const auto& a : p.analyses)
      if (a.uuid == uuid) {
        position = &p;
        analysis = &a;
      }
  }
  if (!analysis) return false;
  // The state as fit_level decides it, with a fit or without one.
  const auto state_under = [&](const pp::Edits& edits) {
    const auto evaluated = pp::evaluate_position(*position, inputs_->monitor_set, options_, edits);
    for (const auto& a : evaluated.analyses)
      if (a.uuid == uuid) return a.state;
    return pp::AnalysisState::NotReduced;
  };
  const auto unusable = [](pp::AnalysisState s) {
    return s == pp::AnalysisState::NotReduced || s == pp::AnalysisState::NoJ;
  };
  const pp::AnalysisState now = state_under(edits_);
  if (unusable(now)) return false;
  const bool used = now == pp::AnalysisState::Used;
  const bool want = use.value_or(!used);
  if (want == used) return false;  // as it is: a box clicked twice does not net to something else
  // Without a word of ours about it, then the word only if it is needed: an
  // edit undone leaves nothing behind.
  edits_.omit.erase(analysis->record_id);
  edits_.include.erase(analysis->record_id);
  const pp::AnalysisState bare = state_under(edits_);
  if (!unusable(bare) && (bare == pp::AnalysisState::Used) != want)
    (want ? edits_.include : edits_.omit).insert(analysis->record_id);
  return true;
}

void FluxWindow::set_in_fit(int hole, bool in_fit) {
  const pp::LevelPosition* position = inputs_ ? monitor_at(*inputs_, hole) : nullptr;
  if (!position) return;
  pp::Edits next = edits_;
  next.exclude_positions.erase(hole);
  next.include_positions.erase(hole);
  // Whether the saved fit's exclusion is carried, as fit_level decides it.
  const bool excluded = pp::evaluate_position(*position, inputs_->monitor_set, options_, next).excluded;
  if (excluded == in_fit) (in_fit ? next.include_positions : next.exclude_positions).insert(hole);
  if (next.exclude_positions == edits_.exclude_positions && next.include_positions == edits_.include_positions) return;
  edits_ = std::move(next);
  forget_message();
  request_fit();
  refresh_status();
}

void FluxWindow::set_save(int hole, bool save) {
  if (!inputs_) return;
  forget_message();
  if (save)
    skip_.erase(hole);
  else
    skip_.insert(hole);
  apply_skip();
  refresh_status();
}

void FluxWindow::apply_skip() {
  resetting_ = true;
  monitors_->set_skip(skip_);
  unknowns_->set_skip(skip_);
  restore_selection();
  resetting_ = false;
}

void FluxWindow::revert() {
  if (!inputs_) return;
  forget_message();
  if (group_edited()) {
    // The other monitors are an edit that only the store undoes: the level as
    // its saved fit chose them, and with that everything else as loaded.
    chosen_.reset();
    return start_load();
  }
  edits_ = {};
  skip_.clear();
  loaded_skip_.clear();
  apply_skip();
  values_ = loaded_values_;
  editor_->set_options(values_);
  if (!loaded_preset_.isEmpty()) preset_name_ = loaded_preset_;
  show_preset();
  resolve_options();
  show_group(shown_);  // a sample typed and not yet asked for
  fit_now();
}

void FluxWindow::reset_omissions() {
  if (!inputs_) return;
  forget_message();
  edits_ = {};
  // Only when the saved fit left something out: else there is nothing to
  // forget, and the flag would be an edit that changes nothing.
  pp::Edits reset;
  reset.reset_omits = true;
  for (const auto& p : inputs_->positions) {
    if (!p.monitor || !p.saved || edits_.reset_omits) continue;
    const auto carried = pp::evaluate_position(p, inputs_->monitor_set, options_, edits_);
    const auto bare = pp::evaluate_position(p, inputs_->monitor_set, options_, reset);
    if (carried.excluded != bare.excluded) edits_.reset_omits = true;
    for (std::size_t i = 0; i < carried.analyses.size() && i < bare.analyses.size(); ++i)
      if (carried.analyses[i].state != bare.analyses[i].state) edits_.reset_omits = true;
  }
  fit_now();
}

// ---- Edits pending (section 5.6) --------------------------------------------

FluxWindow::Unsaved FluxWindow::ask() {
  asking_ = true;
  const Unsaved answer = ask_unsaved_(tr("Save the flux of %1 %2?").arg(irradiation_, level_));
  asking_ = false;
  return answer;
}

void FluxWindow::leave(bool with_group, std::function<void()> next, const std::function<void()>& stay) {
  if (!pending(with_group)) return next();
  switch (ask()) {
    case Unsaved::Discard:
      return next();
    case Unsaved::Save:
      // What was asked for happens once the level is saved; until then the window is as it was.
      if (stay) stay();
      return save_then(std::move(next));
    case Unsaved::Cancel:
      if (stay) stay();
      return;
  }
}

// ---- Saving (section 5.5) ---------------------------------------------------

void FluxWindow::save() { save_then({}); }

QString FluxWindow::save_text(const pp::FluxSaveOutcome& outcome, const std::string& saved_by,
                              const std::string& saved_utc) {
  if (outcome.conflict) {
    const QString position = QString::fromStdString(outcome.conflict_position);
    const QString by = saved_by.empty() ? tr("someone else") : QString::fromStdString(saved_by);
    return saved_utc.empty()
               ? tr("Not saved: %1 was saved by %2 since this level was loaded. Reload and fit again.").arg(position, by)
               : tr("Not saved: %1 was saved by %2 at %3 UTC since this level was loaded. Reload and fit again.")
                     .arg(position, by, QString::fromStdString(saved_utc));
  }
  QString text = outcome.written > 0
                     ? tr("Saved %1 positions (%2 unchanged)").arg(outcome.written).arg(outcome.unchanged)
                     : tr("Nothing to save: %1 positions unchanged").arg(outcome.unchanged);
  if (outcome.skipped > 0) text = tr("%1, %2 not saved").arg(text).arg(outcome.skipped);
  return text;
}

QString FluxWindow::save_error_text(const std::string& what) {
  return tr("Not saved: %1").arg(QString::fromStdString(what));
}

void FluxWindow::commit_typed() {
  // A spin box takes what was typed into it; a field that commits when it is
  // left is left. Both say "changed" now, before the fit that is saved.
  // The sample field is not left: what is typed there is other monitors, not
  // this fit's (save_then decides what becomes of it).
  for (QAbstractSpinBox* spin : dock_host_->findChildren<QAbstractSpinBox*>()) spin->interpretText();
  if (QWidget* focus = QApplication::focusWidget(); focus && focus != sample_edit_ && dock_host_->isAncestorOf(focus))
    focus->clearFocus();
}

void FluxWindow::tell(const QString& text, bool error) {
  if (!loading_) return say(text, error);
  note_ = text;  // the level is being read: once it is fitted
  note_error_ = error;
  note_generation_ = load_generation_;
}

void FluxWindow::save_then(std::function<void()> next) {
  if (saving_) {
    // The save in flight is the one asked for: what was asked follows it.
    if (next) after_save_ = std::move(next);
    return;
  }
  if (loading_ || !inputs_) return;
  // In this order: what was typed is committed, then fitted, then copied, and
  // only then do the widgets wait (disabling one that holds the focus commits
  // it, which would be after the fit was taken).
  // What is saved is fit_, which carries its own monitors: the group's
  // widgets are never read into it.
  commit_typed();
  // R13: a sample typed and not yet asked for is committed by a direct Save,
  // and that is a change of the monitor group like any other: the question
  // when edits are pending (whose Save answer comes back here with `next`,
  // the group put back), then the level read with it. This call is done.
  if (!next && !(group_shown() == shown_)) return group_changed();
  if (loading_ || saving_ || !inputs_) return;
  if (fit_timer_->isActive()) fit_now();  // what is saved is what is on show
  if (!fit_) return;                      // the status says why
  after_save_ = std::move(next);

  const std::string software = QStringLiteral("pychron-ui %1").arg(app_version()).trimmed().toStdString();
  // The job takes a copy of the fit and no `this`. Its result is delivered to
  // the bridge, not to the window: a save that was written is told to the
  // other windows also when this one is gone by then.
  std::function<Result<SaveResult>(ps::IStore&, const ps::Actor&)> job =
      [fit = *fit_, skip = skip_, software](ps::IStore& store, const ps::Actor& actor) -> Result<SaveResult> {
    auto outcome = pp::save_level(store, actor, fit, pp::SaveSelection{skip}, software);
    if (!outcome) return fail(outcome.error());
    SaveResult result{std::move(*outcome), {}, {}};
    if (result.outcome.conflict) {
      if (auto head = pp::flux_head_info(store, fit.irradiation, fit.level, result.outcome.conflict_hole)) {
        result.saved_by = std::move(head->saved_by);
        result.saved_utc = std::move(head->saved_utc);
      }
    }
    return result;
  };
  std::function<void(Result<SaveResult>)> done = [self = QPointer<FluxWindow>(this), bridge = &bridge_,
                                                  generation = load_generation_, irradiation = irradiation_,
                                                  level = level_](Result<SaveResult> result) {
    const bool written = result && !result->outcome.conflict && result->outcome.written > 0;
    if (written) {
      // This window reads its level again itself, below.
      if (self) self->notifying_ = true;
      bridge->notify_changed();
      if (self) self->notifying_ = false;
    }
    if (!self) return;
    FluxWindow& w = *self;
    --w.busy_;
    w.saving_ = false;
    std::function<void()> next = std::exchange(w.after_save_, {});
    w.update_enabled();
    w.update_actions();
    // Another level is on show: the result is not its business, but for the tree.
    if (w.irradiation_ != irradiation || w.level_ != level) {
      if (written) w.reload_tree();
      return;
    }
    // The level was asked for again meanwhile: that read follows the save on
    // the worker, so it shows what was saved, and what was to follow is moot.
    const bool reread = generation != w.load_generation_;
    if (reread) next = {};
    const QString text = result ? save_text(result->outcome, result->saved_by, result->saved_utc)
                                : save_error_text(result.error().what);
    const bool failed = !result || result->outcome.conflict.has_value();
    if (failed) return w.tell(text, true);
    if (written) {
      w.reload_tree();
      Q_EMIT w.saved(irradiation, level);
      if (!self) return;
      // The monitors chosen here are the saved fit's now: nothing chosen
      // gives them, and the reload says what that is.
      w.chosen_.reset();
      w.baseline_.reset();
    }
    if (next) {
      // What was asked for takes the window elsewhere: said after its status.
      next();
      if (!self) return;
      w.message_ = text;
      w.message_details_.clear();
      w.update_tooltip();
      return w.refresh_status();
    }
    if (!written) return w.tell(text, false);
    if (!reread) {
      w.kept_skip_ = w.skip_;
      w.start_load();  // the saved J and revisions, for the next save
      w.kept_skip_generation_ = w.load_generation_;
    }
    w.tell(text, false);
  };

  saving_ = true;
  ++busy_;
  update_enabled();
  update_actions();
  status_text_ = tr("Saving…");
  status_error_ = false;
  style::set_tone(status_, style::Tone::Normal);
  status_->setText(status_text_);
  bridge_.run<SaveResult>(&bridge_, std::move(job), std::move(done));
}

// ---- Export -----------------------------------------------------------------

Result<void> FluxWindow::export_csv(const QString& path) {
  if (fit_timer_->isActive()) fit_now();  // what is on show
  if (!fit_) return fail(ErrorKind::Config, tr("Nothing fitted to export").toStdString());
  const std::string text = pp::flux_csv_header() + pp::flux_csv_rows(*fit_);
  // A sibling temporary file renamed over the destination: a file there is whole or as it was.
  QSaveFile file(path);
  const auto size = static_cast<qint64>(text.size());
  if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), size) != size || !file.commit())
    return fail(ErrorKind::Io, tr("could not write %1: %2").arg(path, file.errorString()).toStdString());
  pychron::mark_as_user_file(std::filesystem::path(path.toStdU16String()));
  return {};
}

void FluxWindow::export_asked() {
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Export CSV"), QStringLiteral("flux_%1_%2.csv").arg(irradiation_, level_), tr("CSV files (*.csv)"));
  if (path.isEmpty()) return;
  if (auto written = export_csv(path); !written)
    QMessageBox::warning(this, windowTitle(), QString::fromStdString(written.error().what));
}

void FluxWindow::closeEvent(QCloseEvent* event) {
  if (!edited()) return event->accept();
  switch (ask()) {
    case Unsaved::Discard:
      // Dropped for good: shown again, the window is not still edited (a
      // chosen monitor group is undone by reading the level again).
      revert();
      return event->accept();
    case Unsaved::Save:
      event->ignore();
      // Read again first: closed, the window holds nothing that is pending.
      return save_then([this] {
        start_load();
        close();
      });
    case Unsaved::Cancel:
      return event->ignore();
  }
}

// ---- The monitor group (W7) -------------------------------------------------

const pp::MonitorSet* FluxWindow::monitor_set(const QString& name) const {
  const std::string wanted = name.toStdString();
  for (const auto& set : sets_)
    if (set.name == wanted) return &set;
  return nullptr;
}

void FluxWindow::set_monitor_sets(std::vector<pp::MonitorSet> sets, std::string default_set) {
  sets_ = std::move(sets);
  default_set_ = std::move(default_set);
  syncing_ = true;
  // What is shown stays shown, also a set the document no longer has.
  const QString keep = set_combo_->count() > 0 ? set_combo_->currentText() : QString::fromStdString(default_set_);
  set_combo_->clear();
  for (const auto& set : sets_) set_combo_->addItem(QString::fromStdString(set.name));
  if (set_combo_->findText(keep) < 0 && !keep.isEmpty()) set_combo_->addItem(keep);
  set_combo_->setCurrentIndex(set_combo_->findText(keep));
  update_set_hint();
  syncing_ = false;
  if (!inputs_ && !loading_) shown_ = group_shown();  // no level: nothing was chosen for one
}

void FluxWindow::update_set_hint() {
  const pp::MonitorSet* set = monitor_set(set_combo_->currentText());
  sample_edit_->setPlaceholderText(set ? QString::fromStdString(set->sample) : QString());
  set_combo_->setToolTip(set ? tr("sample %1 · %2 ± %3 Ma · lambda_k %4")
                                   .arg(QString::fromStdString(set->sample))
                                   .arg(set->age_ma, 0, 'g', 6)
                                   .arg(set->age_err_ma, 0, 'g', 3)
                                   .arg(set->lambda_k().value, 0, 'g', 5)
                             : QString());
}

FluxWindow::MonitorGroup FluxWindow::group_shown() const {
  MonitorGroup group;
  group.set = set_combo_->currentText().toStdString();
  group.sample = sample_edit_->text().trimmed().toStdString();
  // An empty field is the set's own sample, said outright: left unsaid, a
  // saved fit's other sample would come back and could never be cleared.
  if (group.sample.empty())
    if (const pp::MonitorSet* set = monitor_set(set_combo_->currentText())) group.sample = set->sample;
  group.all_positions = all_box_->isChecked();
  return group;
}

void FluxWindow::show_group(const MonitorGroup& group) {
  syncing_ = true;
  const QString name = QString::fromStdString(group.set);
  if (set_combo_->findText(name) < 0) set_combo_->addItem(name);
  set_combo_->setCurrentIndex(set_combo_->findText(name));
  const pp::MonitorSet* set = monitor_set(name);
  sample_edit_->setText(set && set->sample == group.sample ? QString() : QString::fromStdString(group.sample));
  all_box_->setChecked(group.all_positions);
  update_set_hint();
  syncing_ = false;
  shown_ = group_shown();
}

void FluxWindow::group_changed() {
  if (syncing_ || asking_ || saving_) return;
  update_set_hint();
  const MonitorGroup group = group_shown();
  if (group == shown_) return;
  if (irradiation_.isEmpty()) {  // no level to read with it
    shown_ = group;
    return;
  }
  // It changes which positions are monitors: the level is read again.
  leave(
      false,
      [this, group] {
        // Chosen back to the level's own: nothing is chosen.
        chosen_ = baseline_ && group == *baseline_ ? std::nullopt : std::optional<MonitorGroup>(group);
        show_group(group);
        start_load();
      },
      [this, back = shown_] { show_group(back); });
}

}  // namespace pychron::ui
