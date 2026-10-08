#include "flux_window.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>

#include "flux_analysis_model.hpp"
#include "flux_monitor_model.hpp"
#include "flux_unknown_model.hpp"
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

Result<std::vector<TreeIrradiation>> read_tree(ps::IStore& store) {
  auto sets = pp::load_monitor_sets(store);
  if (!sets) return fail(sets.error());
  const pp::MonitorSet* standard = sets->sets.find("");
  const std::string monitor_sample = standard ? standard->sample : std::string();

  auto irradiations = store.irradiations();
  if (!irradiations) return fail(irradiations.error());
  std::vector<TreeIrradiation> out;
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
  return out;
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
      analyses_(new FluxAnalysisModel(this)) {
  setObjectName(QStringLiteral("flux_window"));
  resize(1400, 820);

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
  connect(&bridge_, &EntryBridge::changed, this, [this] { reload_tree(); });

  update_title();
  reload_tree();
}

bool FluxWindow::busy() const noexcept { return busy_ > 0 || fit_timer_->isActive(); }

QString FluxWindow::status() const { return status_->text(); }

void FluxWindow::set_status(const QString& text, bool error, const QStringList& warnings) {
  warnings_ = warnings;
  status_error_ = error;
  status_->setText(warnings.isEmpty()       ? text
                   : warnings.size() == 1 ? tr("%1 · 1 warning").arg(text)
                                          : tr("%1 · %2 warnings").arg(text).arg(warnings.size()));
  status_->setToolTip(warnings.join(QLatin1Char('\n')));
  style::set_tone(status_, error ? style::Tone::Error : style::Tone::Normal);
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
  if (!item || item == tree_->currentItem()) return;
  const QSignalBlocker blocked(tree_);
  item->parent()->setExpanded(true);
  tree_->setCurrentItem(item);
}

void FluxWindow::reload_tree() {
  const quint64 generation = ++tree_generation_;
  ++busy_;
  ++tree_jobs_;
  bridge_.run<std::vector<TreeIrradiation>>(
      this, [](ps::IStore& store, const ps::Actor&) { return read_tree(store); },
      [this, generation](Result<std::vector<TreeIrradiation>> read) {
        --busy_;
        --tree_jobs_;
        if (generation != tree_generation_) return;  // a newer read is on its way
        if (!read) return set_status(QString::fromStdString(read.error().what), true);
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
  irradiation_ = irradiation;
  level_ = level;
  select_tree_item();
  if (tree_->topLevelItemCount() == 0 && tree_jobs_ == 0) reload_tree();
  start_load();
}

void FluxWindow::clear_level() {
  fit_timer_->stop();
  resetting_ = true;
  analyses_->set_position(nullptr);
  monitors_->set_fit(nullptr);
  monitors_->set_inputs(nullptr);
  unknowns_->set_fit(nullptr);
  unknowns_->set_inputs(nullptr);
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
  clear_level();
  set_tables_enabled(false);
  set_status(tr("Loading %1 %2…").arg(irradiation_, level_), false);
  ++busy_;
  // The job runs on the worker and may outlive the window: it takes values and
  // the source, never `this`.
  bridge_.run<pp::LevelInputs>(
      this,
      [source = &source_, irradiation = irradiation_.toStdString(), level = level_.toStdString()](
          ps::IStore& store, const ps::Actor&) {
        return pp::load_level(*source, store, irradiation, level, pp::MonitorSelection{});
      },
      [this, generation](Result<pp::LevelInputs> loaded) {
        --busy_;
        if (generation != load_generation_) return;  // superseded by a newer selection
        set_tables_enabled(true);
        if (!loaded) return set_status(QString::fromStdString(loaded.error().what), true);
        apply_loaded(std::move(*loaded));
      });
}

void FluxWindow::apply_loaded(pp::LevelInputs inputs) {
  clear_level();
  inputs_ = std::move(inputs);
  options_ = inputs_->saved_options.value_or(pp::FluxOptions{});
  resetting_ = true;
  monitors_->set_inputs(&*inputs_);
  unknowns_->set_inputs(&*inputs_);
  resetting_ = false;
  update_title();
  fit_now();  // nothing to wait for: the debounce is for edits
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

  auto fitted = pp::fit_level(*inputs_, options_, edits_);
  if (fitted) {
    fit_ = std::move(*fitted);
    monitors_->set_fit(&*fit_);
    unknowns_->set_fit(&*fit_);
  } else {
    fit_error_ = fitted.error().what;
  }

  // The selection is by hole: it survives the reset of the model.
  const int row = selected_hole_ ? monitors_->row_of(*selected_hole_) : -1;
  if (row >= 0)
    monitor_table_->setCurrentIndex(monitors_->index(row, 0));
  else
    selected_hole_.reset();
  resetting_ = false;

  show_selected();
  update_scene();
  if (fit_) {
    QStringList warnings;
    for (const auto& line : pp::flux_warnings(*inputs_, *fit_)) warnings << QString::fromStdString(line);
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

}  // namespace pychron::ui
