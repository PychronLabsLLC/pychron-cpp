#include "flux_window.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "flux_analysis_model.hpp"
#include "flux_monitor_model.hpp"
#include "flux_unknown_model.hpp"
#include "options_editor.hpp"
#include "preset_bar.hpp"
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
constexpr int kMessageMs = 6000;  // how long a preset bar message covers the status

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

QTableView* make_table(QAbstractItemModel* model, const QString& name, QWidget* parent) {
  auto* table = new QTableView(parent);
  table->setObjectName(name);
  table->setModel(model);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  return table;
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
  setCentralWidget(split);

  auto* bar = addToolBar(tr("Flux"));
  bar->setObjectName(QStringLiteral("flux_toolbar"));
  revert_action_ = bar->addAction(tr("Revert"), this, [this] { revert(); });
  revert_action_->setToolTip(tr("Drop the edits and return to the options the level was loaded with"));
  reload_action_ = bar->addAction(tr("Reload"), this, [this] { reload(); });
  reload_action_->setToolTip(tr("Read the level again"));
  reset_action_ = bar->addAction(tr("Reset omissions"), this, [this] { reset_omissions(); });
  reset_action_->setToolTip(tr("Forget what the saved fit omitted and excluded; tags still apply"));

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
  connect(analyses_, &FluxAnalysisModel::use_toggled, this, [this](const QString& uuid, bool) { toggle_analyses({uuid}); });
  connect(monitors_, &FluxMonitorModel::fit_toggled, this, [this](int hole, bool in_fit) { set_in_fit(hole, in_fit); });
  connect(monitors_, &FluxMonitorModel::save_toggled, this, [this](int hole, bool save) { set_save(hole, save); });
  connect(unknowns_, &FluxUnknownModel::save_toggled, this, [this](int hole, bool save) { set_save(hole, save); });
  // Someone else's change (this window saves nothing yet): the tree always;
  // the level only when no edit would be lost with it.
  connect(&bridge_, &EntryBridge::changed, this, [this] {
    reload_tree();
    if (irradiation_.isEmpty()) return;
    if (!edited()) return start_load();
    changed_elsewhere_ = true;
    refresh_status();
  });

  update_title();
  update_actions();
  reload_tree();
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
  addDockWidget(Qt::RightDockWidgetArea, dock);

  editor_->set_options(values_);
  preset_bar_->reload(preset_name_);
  connect(preset_bar_, &PresetBar::loaded, this, [this](const pp::Options& options, const QString& name) {
    preset_name_ = name;
    set_options(options);
  });
  connect(preset_bar_, &PresetBar::pinned_chosen, this, [this] { set_options(loaded_values_); });
  connect(preset_bar_, &PresetBar::message, this, [this](const QString& text, const QString& details) {
    statusBar()->showMessage(details.isEmpty() ? text : tr("%1: %2").arg(text, details), kMessageMs);
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
  status_->setToolTip(warnings.join(QLatin1Char('\n')));
  style::set_tone(status_, error ? style::Tone::Error : style::Tone::Normal);
  refresh_status();
}

void FluxWindow::refresh_status() {
  QString text = status_text_;
  if (inputs_ && changed_elsewhere_) text = tr("%1 · level changed elsewhere, Reload to see it").arg(text);
  if (edited()) text = tr("%1 · edited (not saved)").arg(text);
  status_->setText(text);
}

void FluxWindow::update_actions() {
  reload_action_->setEnabled(!loading_ && !irradiation_.isEmpty());
  revert_action_->setEnabled(!loading_ && inputs_.has_value());
  reset_action_->setEnabled(!loading_ && inputs_.has_value());
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

void FluxWindow::set_tables_enabled(bool enabled) {
  for (QTableView* table : {monitor_table_, analysis_table_, unknown_table_}) table->setEnabled(enabled);
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
        if (!tree) return set_status(QString::fromStdString(tree.error().what), true);
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
  monitors_->set_fit(nullptr);
  monitors_->set_inputs(nullptr);
  unknowns_->set_fit(nullptr);
  unknowns_->set_inputs(nullptr);
  skip_.clear();
  monitors_->set_skip(skip_);
  unknowns_->set_skip(skip_);
  resetting_ = false;
  fit_.reset();
  unfitted_.reset();
  inputs_.reset();
  fit_error_.clear();
  selected_hole_.reset();
  edits_ = {};
  view_->set_scene(nullptr);
  update_title();
}

void FluxWindow::start_load() {
  const quint64 generation = ++load_generation_;
  reselect_ = selected_hole_;  // read again, the level keeps its selected monitor
  clear_level();
  loading_ = true;
  changed_elsewhere_ = false;
  set_tables_enabled(false);
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
        set_tables_enabled(true);
        update_actions();
        if (!loaded) return set_status(QString::fromStdString(loaded.error().what), true);
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

  selected_hole_ = reselect_;
  resetting_ = true;
  monitors_->set_inputs(&*inputs_);
  unknowns_->set_inputs(&*inputs_);
  resetting_ = false;
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
  monitors_->set_fit(nullptr);
  unknowns_->set_fit(nullptr);
  fit_.reset();
  unfitted_.reset();
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

  // The selection is by hole: it survives the reset of the model.
  restore_selection();
  resetting_ = false;

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
    return;
  }
  // No fit: the analyses as fit_level counts them, so what is wrong can be seen.
  const auto found = std::find_if(inputs_->positions.begin(), inputs_->positions.end(),
                                  [&](const pp::LevelPosition& p) { return p.hole == *selected_hole_; });
  if (found == inputs_->positions.end() || !found->monitor) return;
  unfitted_ = pp::evaluate_position(*found, inputs_->monitor_set, options_, edits_);
  analyses_->set_position(&*unfitted_);
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
  return !no_edits(edits_) || !skip_.empty() || !same_values(values_, loaded_values_) ||
         (with_group && !(group_shown() == shown_));
}

bool FluxWindow::edited() const noexcept { return pending(true); }

void FluxWindow::toggle_analyses(const QStringList& uuids) {
  if (!inputs_) return;
  bool changed = false;
  QSet<QString> seen;  // a rubber band over a highlighted monitor names its analyses twice
  for (const QString& id : uuids) {
    if (seen.contains(id)) continue;
    seen.insert(id);
    const std::string uuid = id.toStdString();
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
    if (!analysis) continue;
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
    if (unusable(now)) continue;
    const bool use = now != pp::AnalysisState::Used;
    // Without a word of ours about it, then the word only if it is needed: an
    // edit undone leaves nothing behind.
    edits_.omit.erase(analysis->record_id);
    edits_.include.erase(analysis->record_id);
    const pp::AnalysisState bare = state_under(edits_);
    if (!unusable(bare) && (bare == pp::AnalysisState::Used) != use)
      (use ? edits_.include : edits_.omit).insert(analysis->record_id);
    changed = true;
  }
  if (!changed) return;
  request_fit();
  refresh_status();
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
  request_fit();
  refresh_status();
}

void FluxWindow::set_save(int hole, bool save) {
  if (!inputs_) return;
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
  edits_ = {};
  skip_.clear();
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
  edits_ = {};
  edits_.reset_omits = true;
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

void FluxWindow::save_then(std::function<void()> /*next*/) {
  // Saving is the next task's (plan task 8): it saves the level and calls `next` when that succeeded.
}

void FluxWindow::closeEvent(QCloseEvent* event) {
  if (!edited()) return event->accept();
  switch (ask()) {
    case Unsaved::Discard:
      return event->accept();
    case Unsaved::Save:
      event->ignore();
      return save_then([this] { close(); });
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
  if (syncing_ || asking_) return;
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
        chosen_ = group;
        show_group(group);
        start_load();
      },
      [this, back = shown_] { show_group(back); });
}

}  // namespace pychron::ui
