#include "figure_window.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "domain_pipeline.hpp"
#include "error_report.hpp"
#include "options_editor.hpp"
#include "preset_bar.hpp"
#include "r_values_panel.hpp"
#include "pychron/arar/arar_figures.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/arar/r_values.hpp"
#include "pychron/arar/report.hpp"
#include "scene_view.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::vector<std::string> to_std(const QStringList& l) {
  std::vector<std::string> out;
  for (const auto& s : l) out.push_back(s.toStdString());
  return out;
}

constexpr const char* kFigure = "figure";
constexpr const char* kEdits = "edits";
constexpr const char* kGroup = "group";
// A second group unit, at the graph level: it puts each analysis on a graph.
constexpr const char* kGraph = "graph";

// Columns of the analyses dock.
enum Column : int { kRunIdCol, kTypeCol, kGroupCol, kColorCol, kIncludedCol, kColumnCount };

}  // namespace

QString FigureWindow::same_as_group() { return QStringLiteral("same as group"); }

namespace {

// The domain's time_series figure; a domain without one is a mistake in the
// calling code.
pp::FigureEntry time_series_entry(const pp::Domain& domain) {
  const auto figures = domain.figures();
  const auto it =
      std::find_if(figures.begin(), figures.end(), [](const pp::FigureEntry& e) { return e.kind == "time_series"; });
  if (it == figures.end()) throw std::logic_error("domain " + std::string(domain.id()) + " has no time_series figure");
  return *it;
}

}  // namespace

FigureWindow::FigureWindow(ProcessingBridge& bridge, pp::PresetStore& presets, const QStringList& uuids, QWidget* parent)
    : FigureWindow(bridge, presets, time_series_entry(bridge.domain()), uuids, parent) {}

FigureWindow::FigureWindow(ProcessingBridge& bridge, pp::PresetStore& presets, const pp::FigureEntry& entry,
                           const QStringList& uuids, QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      store_(presets),
      kind_(entry.kind),
      schema_(bridge.domain().units().find(kind_)->schema()),
      channel_(bridge.new_channel()) {
  const auto& reg = bridge.domain().units();
  const auto* unit = reg.find(kind_);
  setWindowTitle(qs(std::string(unit->title())));  // update_title() adds what is on it
  resize(1200, 800);
  ask_preset_name = [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save preset"), tr("Name"), QLineEdit::Normal, QString(), &ok);
    return ok ? name.trimmed() : QString();
  };

  auto& select = pipeline_.add(reg, "select", "select");
  (void)select.options.set("uuids", to_std(uuids));
  (void)select.options.set("remove_tags", std::vector<std::string>{});  // the user picked these
  const std::string last = add_reduction_chain(pipeline_, reg, bridge.domain(), "select");
  pipeline_.add(reg, kGroup, "group", {last});
  auto& graph = pipeline_.add(reg, kGraph, "group", {kGroup});
  (void)graph.options.set("level", std::string("graph"));
  pipeline_.add(reg, kEdits, "edits", {kGraph});
  (void)pipeline_.find(kGroup)->options.set("key", entry.group_key);
  auto& fig = pipeline_.add(reg, kFigure, kind_, {kEdits});
  fig.options = store_.defaults(schema_);
  fig.preset = "Default";

  view_ = new SceneView;
  setCentralWidget(view_);
  connect(view_, &SceneView::point_clicked, this, [this](const QString& id) { toggle_exclusion({id}); });
  connect(view_, &SceneView::points_toggled, this, [this](const QStringList& ids) { toggle_exclusion(ids); });
  connect(view_, &SceneView::recall_requested, this, &FigureWindow::recall_requested);
  connect(view_, &SceneView::use_limits_requested, this,
          [this](const SceneView::ViewLimits& limits) { use_view_limits(limits); });

  auto* tools = addToolBar(tr("Figure"));
  tools->addWidget(new QLabel(tr(" Group by ")));
  group_ = new QComboBox;
  for (const auto& c : reg.find("group")->schema()->field("key")->choices) group_->addItem(qs(c));
  group_->setCurrentText(qs(entry.group_key));
  tools->addWidget(group_);
  connect(group_, &QComboBox::currentTextChanged, this, [this](const QString& k) { set_group_key(k); });
  tools->addWidget(new QLabel(tr("  Graph by ")));
  graph_ = new QComboBox;
  graph_->addItem(QStringLiteral("none"));
  graph_->addItem(same_as_group());
  for (const auto& c : reg.find("group")->schema()->field("key")->choices)
    if (c != "none") graph_->addItem(qs(c));
  graph_->setToolTip(tr("Which groups share a graph: none puts them all on one, \"same as group\" gives each its own, "
                        "and a key such as sample draws the groups of one sample together"));
  tools->addWidget(graph_);
  connect(graph_, &QComboBox::currentTextChanged, this, [this](const QString& k) { set_graph_key(k); });
  tools->addSeparator();
  tools->addAction(tr("Reset view"), view_, &SceneView::reset_view);
  tools->addAction(tr("Export..."), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export figure"), QString(), tr("PDF (*.pdf);;PNG (*.png)"));
    if (!path.isEmpty() && !export_figure(path)) show_warning(this, "figure", tr("Export"), tr("Could not write %1").arg(path));
  });
  auto* table = tools->addAction(tr("Export table..."), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export data report"), QStringLiteral("report.csv"),
                                                      tr("CSV report (*.csv);;JSON report (*.json)"));
    if (!path.isEmpty() && !export_report(path)) show_warning(this, "figure", tr("Export"), tr("Could not write %1").arg(path));
  });
  table->setToolTip(tr("Write these analyses as a 40Ar/39Ar data report after Schaen et al. (2021)"));

  // Options dock: presets + generated editor.
  auto* dock = new QDockWidget(tr("Options"), this);
  auto* host = new QWidget;
  auto* hl = new QVBoxLayout(host);
  presets_ = new PresetBar(store_, schema_);
  presets_->current = [this] { return pipeline_.find(kFigure)->options; };
  presets_->ask_name = [this] { return ask_preset_name ? ask_preset_name() : QString(); };
  hl->addWidget(presets_);
  editor_ = new OptionsEditor;
  hl->addWidget(editor_, 1);
  dock->setWidget(host);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  connect(presets_, &PresetBar::loaded, this, [this](const pp::Options& o, const QString& name) {
    pipeline_.find(kFigure)->preset = name.toStdString();
    set_figure_options(o);
  });
  connect(presets_, &PresetBar::message, this, [this](const QString& text, const QString& details) {
    status_->setText(text);
    if (!details.isEmpty()) status_->setToolTip(details);
  });
  connect(editor_, &OptionsEditor::changed, this, [this] {
    pipeline_.find(kFigure)->options = editor_->options();
    schedule();
  });

  // Analyses dock.
  auto* adock = new QDockWidget(tr("Analyses"), this);
  analyses_ = new QTableWidget;
  analyses_->setColumnCount(kColumnCount);
  analyses_->setHorizontalHeaderLabels({tr("Run ID"), tr("Type"), tr("Group"), tr("Color"), tr("Included")});
  analyses_->verticalHeader()->setVisible(false);
  analyses_->horizontalHeader()->setStretchLastSection(true);
  adock->setWidget(analyses_);
  addDockWidget(Qt::BottomDockWidgetArea, adock);
  connect(analyses_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (filling_ || item->column() != kIncludedCol) return;
    const QString id = item->data(Qt::UserRole).toString();
    toggle_exclusion({id});
  });
  connect(analyses_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    if (auto* item = analyses_->item(row, kIncludedCol)) emit recall_requested(item->data(Qt::UserRole).toString());
  });

  status_ = new QLabel;
  statusBar()->addWidget(status_, 1);

  if (kind_ == "ideogram") {
    r_values_dock_ = new QDockWidget(tr("R-values"), this);
    r_values_dock_->setObjectName(QStringLiteral("r_values"));
    r_values_ = new RValuesPanel;
    r_values_dock_->setWidget(r_values_);
    addDockWidget(Qt::BottomDockWidgetArea, r_values_dock_);
    splitDockWidget(adock, r_values_dock_, Qt::Horizontal);  // beside the analyses, not a tab
    r_values_dock_->hide();
  }

  debounce_.setSingleShot(true);
  debounce_.setInterval(150);
  connect(&debounce_, &QTimer::timeout, this, &FigureWindow::run);

  editor_->set_options(pipeline_.find(kFigure)->options);
  presets_->reload(QStringLiteral("Default"));
  run();
}

// The axis limits among a figure's options, as text to compare: every `min`,
// `max`, `y_min` and `y_max` that is set, under its table (`[x]`,
// `[[panels]]`, `[isochron.x]`, ...), in the order the options are written.
QString FigureWindow::limits_of(const pp::Options& options) {
  static const QRegularExpression limit(QStringLiteral("^(min|max|y_min|y_max)\\s*="));
  QString out;
  QString table;
  const QString text = qs(pp::options_to_toml(options));
  for (const auto& raw : text.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('[')))
      table = line;
    else if (limit.match(line).hasMatch())
      out += table + QLatin1Char(' ') + line + QLatin1Char('\n');
  }
  return out;
}

FigureWindow::~FigureWindow() { bridge_.release(channel_); }

void FigureWindow::schedule() { debounce_.start(); }

void FigureWindow::run() {
  debounce_.stop();
  status_->setText(tr("Computing..."));
  bridge_.submit(channel_, pipeline_, {kFigure, kEdits}, [this](const PipelineResult& r) { on_result(r); });
}

void FigureWindow::on_result(const PipelineResult& r) {
  ++runs_;
  QStringList warnings;
  for (const auto& d : r.diagnostics) warnings << qs(d);
  if (r.outputs.size() >= 2 && r.outputs[1]) {
    dataset_ = std::get<pp::DatasetPtr>(r.outputs[1]->at(0));
    fill_analyses();
    update_title();
    if (!quantities_set_ && dataset_) {
      std::vector<pp::ReducedPtr> v;
      for (const auto& it : dataset_->items()) v.push_back(it.analysis);
      QStringList q;
      for (const auto& s : pp::available_quantities(v, bridge_.domain().quantities())) q << qs(s);
      editor_->set_quantity_choices(q);
      quantities_set_ = true;
    }
  }
  if (r.outputs.empty() || !r.outputs[0]) {
    const QString what = r.outputs.empty() ? tr("nothing computed") : qs(to_string(r.outputs[0].error()));
    log_shown("figure", tr("Compute"), what);
    status_->setText(tr("Error: %1").arg(what));
    view_->set_scene(nullptr);
    if (r_values_dock_) r_values_dock_->hide();
    r_values_shown_ = false;
    emit figure_updated();
    return;
  }
  auto scene = std::get<pp::ScenePtr>(r.outputs[0]->at(0));
  for (const auto& w : scene->warnings) warnings << qs(w);
  view_->set_scene(scene);
  // The view keeps its ranges across a rerun; a limit set, changed or taken
  // away in the options is a view asked for, and wins.
  if (QString limits = limits_of(pipeline_.find(kFigure)->options); limits != shown_limits_) {
    if (shown_limits_) view_->reset_view();
    shown_limits_ = std::move(limits);
  }
  const int n = dataset_ ? static_cast<int>(dataset_->size()) : 0;
  status_->setText(tr("%1 analyses · %2 s").arg(n).arg(r.seconds, 0, 'f', 2) +
                   (warnings.isEmpty() ? QString() : tr(" · %n warning(s)", nullptr, static_cast<int>(warnings.size()))));
  status_->setToolTip(warnings.join(QLatin1Char('\n')));
  update_r_values();
  emit figure_updated();
}

void FigureWindow::update_r_values() {
  if (!r_values_) return;
  const auto& options = pipeline_.find(kFigure)->options;
  bool tables = false;
  if (dataset_ && options.get_bool("show_rvalues")) {
    if (auto matrices = pp::r_value_matrices(*dataset_, options)) {
      r_values_->set_matrices(std::move(*matrices), options);
      tables = r_values_->has_tables();
    }
  }
  // Opened when there come to be tables; after that, left as the user has it.
  if (!tables)
    r_values_dock_->hide();
  else if (!r_values_shown_)
    r_values_dock_->show();
  r_values_shown_ = tables;
}

// "<plot type> — <sample> (<identifier>)", and how many more identifiers
// there are when the figure holds several.
void FigureWindow::update_title() {
  const QString kind = qs(std::string(bridge_.domain().units().find(kind_)->title()));
  QStringList names;
  if (dataset_) {
    for (const auto& it : dataset_->items()) {
      const auto& a = *it.analysis->analysis;
      if (a.identifier.empty() && a.sample.empty()) continue;
      const QString id = qs(a.identifier);
      const QString name = a.sample.empty()       ? id
                           : a.identifier.empty() ? qs(a.sample)
                                                  : tr("%1 (%2)").arg(qs(a.sample), id);
      if (!names.contains(name)) names << name;
    }
  }
  if (names.isEmpty()) {
    setWindowTitle(kind);
    return;
  }
  const int more = static_cast<int>(names.size()) - 1;
  setWindowTitle(tr("%1 — %2").arg(kind, names.front()) + (more > 0 ? tr(" + %n more", nullptr, more) : QString()));
}

void FigureWindow::fill_analyses() {
  filling_ = true;
  analyses_->setRowCount(dataset_ ? static_cast<int>(dataset_->size()) : 0);
  if (dataset_) {
    const pp::Options& fig = pipeline_.find(kFigure)->options;
    int row = 0;
    for (const auto& it : dataset_->items()) {
      const auto& a = *it.analysis->analysis;
      analyses_->setItem(row, kRunIdCol, new QTableWidgetItem(qs(a.runid)));
      analyses_->setItem(row, kTypeCol, new QTableWidgetItem(qs(a.analysis_type)));
      analyses_->setItem(row, kGroupCol, new QTableWidgetItem(qs(dataset_->group_name(it.path.group))));
      // The colour the figure draws the analysis's group in.
      const pp::Color c = pp::group_color(fig, it.path.group);
      auto* color = new QTableWidgetItem;
      color->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      color->setData(Qt::DecorationRole, QColor(c.r, c.g, c.b));
      color->setToolTip(QColor(c.r, c.g, c.b).name());
      analyses_->setItem(row, kColorCol, color);
      auto* inc = new QTableWidgetItem;
      inc->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      inc->setCheckState(it.exclusion.included() ? Qt::Checked : Qt::Unchecked);
      inc->setData(Qt::UserRole, qs(a.uuid));
      QString why;
      if (it.exclusion.user) why = *it.exclusion.user ? tr("excluded by you") : tr("included by you");
      else if (it.exclusion.filter) why = tr("filtered");
      else if (it.exclusion.tag) why = tr("tag: %1").arg(qs(a.tag));
      inc->setText(why);
      analyses_->setItem(row, kIncludedCol, inc);
      ++row;
    }
  }
  analyses_->resizeColumnsToContents();
  filling_ = false;
}

void FigureWindow::toggle_exclusion(const QStringList& uuids) {
  auto& o = pipeline_.find(kEdits)->options;
  auto ex = o.get_strings("exclude"), inc = o.get_strings("include");
  for (const auto& qid : uuids) {
    const std::string id = qid.toStdString();
    bool excluded_now = false;
    bool tag_or_filter = false;
    if (dataset_)
      for (const auto& it : dataset_->items())
        if (it.analysis->analysis->uuid == id) {
          excluded_now = it.exclusion.excluded();
          tag_or_filter = it.exclusion.tag || it.exclusion.filter;
        }
    ex.erase(std::remove(ex.begin(), ex.end(), id), ex.end());
    inc.erase(std::remove(inc.begin(), inc.end(), id), inc.end());
    if (excluded_now) {
      if (tag_or_filter) inc.push_back(id);  // force in over the tag or filter
    } else {
      ex.push_back(id);
    }
  }
  (void)o.set("exclude", ex);
  (void)o.set("include", inc);
  run();
}

void FigureWindow::set_figure_options(const pp::Options& options) {
  pipeline_.find(kFigure)->options = options;
  editor_->set_options(options);
  run();
}

bool FigureWindow::use_view_limits(const SceneView::ViewLimits& limits) {
  pp::Options options = pipeline_.find(kFigure)->options;
  const auto has = [](const pp::Schema& schema, std::string_view key) {
    return std::any_of(schema.fields.begin(), schema.fields.end(),
                       [key](const pp::FieldSpec& f) { return f.key == key; });
  };
  // An axis drawn reversed has its ends the other way round.
  const auto put = [](pp::Options& o, const char* min_key, const char* max_key, double a, double b) {
    return o.set(min_key, std::min(a, b)).has_value() && o.set(max_key, std::max(a, b)).has_value();
  };
  bool written = false;
  const pp::Schema& schema = *schema_;
  if (has(schema, "x.min") && has(schema, "x.max"))
    written = put(options, "x.min", "x.max", limits.x_min, limits.x_max) || written;
  // A figure of one panel keeps its y limits beside the x ones.
  if (has(schema, "y.min") && has(schema, "y.max") && limits.panels.size() == 1)
    written = put(options, "y.min", "y.max", limits.panels[0].y_min, limits.panels[0].y_max) || written;
  // One of several panels keeps them in its row: panel "p<n>" is row n.
  const auto list = std::find_if(schema.lists.begin(), schema.lists.end(),
                                 [](const pp::ListSpec& l) { return l.key == "panels"; });
  if (list != schema.lists.end() && list->row && has(*list->row, "y_min") && has(*list->row, "y_max")) {
    auto rows = options.rows("panels");
    bool any = false;
    for (const auto& panel : limits.panels) {
      if (panel.id.size() < 2 || panel.id.front() != 'p') continue;
      bool ok = false;
      const int row = QString::fromStdString(panel.id.substr(1)).toInt(&ok);
      if (!ok || row < 0 || std::cmp_greater_equal(row, rows.size())) continue;
      any = put(rows[static_cast<std::size_t>(row)], "y_min", "y_max", panel.y_min, panel.y_max) || any;
    }
    if (any && options.set_rows("panels", std::move(rows)).has_value()) written = true;
  }
  if (!written) {
    status_->setText(tr("This figure's options have no limits to set"));
    return false;
  }
  set_figure_options(options);
  return true;
}

void FigureWindow::set_group_key(const QString& key) {
  if (group_->currentText() != key) {
    group_->setCurrentText(key);  // re-enters through currentTextChanged
    return;
  }
  (void)pipeline_.find(kGroup)->options.set("key", key.toStdString());
  apply_graph_key();
  run();
}

void FigureWindow::set_graph_key(const QString& key) {
  if (graph_->currentText() != key) {
    if (graph_->findText(key) < 0) return;
    graph_->setCurrentText(key);  // re-enters through currentTextChanged
    return;
  }
  apply_graph_key();
  run();
}

void FigureWindow::apply_graph_key() {
  const QString key = graph_->currentText() == same_as_group() ? group_->currentText() : graph_->currentText();
  (void)pipeline_.find(kGraph)->options.set("key", key.toStdString());
}

QComboBox* FigureWindow::preset_combo() const noexcept { return presets_->combo(); }

void FigureWindow::select_preset(const QString& name) { presets_->select(name); }

bool FigureWindow::export_figure(const QString& path) {
  if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) return view_->save_pdf(path);
  return view_->save_png(path);
}

bool FigureWindow::export_report(const QString& path) {
  if (!dataset_) return false;
  pp::ReportOptions options;
  if (kind_ == "spectrum" || kind_ == "spectrum_isochron") {
    namespace r = pychron::reduction;
    const pp::Options& o = pipeline_.find(kFigure)->options;
    // The pair keeps the spectrum's settings under "spectrum.".
    const std::string in = kind_ == "spectrum" ? "" : "spectrum.";
    options.plateau.method = o.get_string(in + "plateau.method") == "mahon" ? r::PlateauMethod::Mahon : r::PlateauMethod::Fleck;
    options.plateau.nsteps = static_cast<int>(o.get_int(in + "plateau.nsteps"));
    options.plateau.gas_fraction = o.get_double(in + "plateau.gas_fraction");
    options.plateau.overlap_sigma = o.get_double(in + "plateau.overlap_sigma");
    options.plateau_weighting = o.get_string(in + "plateau.weighting") == "volume_fraction"
                                    ? r::PlateauWeighting::VolumeFraction
                                    : r::PlateauWeighting::InverseVariance;
    options.mean_error = r::parse_mean_error_kind(o.get_string(in + "plateau.error_kind")).value_or(r::MeanErrorKind::Msem);
    options.integrated_includes_excluded = o.get_bool(in + "integrated.include_excluded");
  }
  const auto report = pp::make_report(*dataset_, options);
  auto saved = pp::save_report(report, std::filesystem::path(path.toStdString()));
  if (!saved) {
    log_shown("figure", tr("Export"), saved.error());
    status_->setText(tr("Export failed: %1").arg(qs(saved.error().what)));
    return false;
  }
  status_->setText(tr("Wrote %1").arg(path));
  return true;
}

}  // namespace pychron::ui
