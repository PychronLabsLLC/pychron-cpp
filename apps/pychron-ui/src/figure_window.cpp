#include "figure_window.hpp"

#include <algorithm>

#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "options_editor.hpp"
#include "preset_bar.hpp"
#include "pychron/processing/quantity.hpp"
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

}  // namespace

QString FigureWindow::default_group_key(const std::string& kind) {
  if (kind == "ideogram") return QStringLiteral("identifier");
  if (kind == "spectrum" || kind == "inverse_isochron") return QStringLiteral("aliquot");
  return QStringLiteral("none");
}

FigureWindow::FigureWindow(ProcessingBridge& bridge, pp::PresetStore& presets, QStringList uuids, QWidget* parent)
    : FigureWindow(bridge, presets, "time_series", std::move(uuids), parent) {}

FigureWindow::FigureWindow(ProcessingBridge& bridge, pp::PresetStore& presets, std::string kind, QStringList uuids,
                           QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      store_(presets),
      kind_(std::move(kind)),
      schema_(pp::UnitRegistry::builtin().find(kind_)->schema()),
      channel_(bridge.new_channel()) {
  const auto* unit = pp::UnitRegistry::builtin().find(kind_);
  setWindowTitle(tr("%1 — %n analyses", nullptr, static_cast<int>(uuids.size())).arg(qs(std::string(unit->title()))));
  resize(1200, 800);
  ask_preset_name = [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save preset"), tr("Name"), QLineEdit::Normal, QString(), &ok);
    return ok ? name.trimmed() : QString();
  };

  const auto& reg = pp::UnitRegistry::builtin();
  auto& select = pipeline_.add(reg, "select", "select");
  (void)select.options.set("uuids", to_std(uuids));
  (void)select.options.set("remove_tags", std::vector<std::string>{});  // the user picked these
  pipeline_.add(reg, "reduce", "reduce", {"select"});
  pipeline_.add(reg, "group", "group", {"reduce"});
  pipeline_.add(reg, kEdits, "edits", {"group"});
  (void)pipeline_.find("group")->options.set("key", default_group_key(kind_).toStdString());
  auto& fig = pipeline_.add(reg, kFigure, kind_, {kEdits});
  fig.options = store_.defaults(schema_);
  fig.preset = "Default";

  view_ = new SceneView;
  setCentralWidget(view_);
  connect(view_, &SceneView::point_clicked, this, [this](const QString& id) { toggle_exclusion({id}); });
  connect(view_, &SceneView::points_toggled, this, [this](const QStringList& ids) { toggle_exclusion(ids); });
  connect(view_, &SceneView::recall_requested, this, &FigureWindow::recall_requested);

  auto* tools = addToolBar(tr("Figure"));
  tools->addWidget(new QLabel(tr(" Group by ")));
  group_ = new QComboBox;
  for (const auto& c : reg.find("group")->schema()->field("key")->choices) group_->addItem(qs(c));
  group_->setCurrentText(default_group_key(kind_));
  tools->addWidget(group_);
  connect(group_, &QComboBox::currentTextChanged, this, [this](const QString& k) { set_group_key(k); });
  tools->addSeparator();
  tools->addAction(tr("Reset view"), view_, &SceneView::reset_view);
  tools->addAction(tr("Export..."), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export figure"), QString(), tr("PDF (*.pdf);;PNG (*.png)"));
    if (!path.isEmpty() && !export_figure(path)) QMessageBox::warning(this, tr("Export"), tr("Could not write %1").arg(path));
  });

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
  analyses_->setColumnCount(4);
  analyses_->setHorizontalHeaderLabels({tr("Run ID"), tr("Type"), tr("Group"), tr("Included")});
  analyses_->verticalHeader()->setVisible(false);
  analyses_->horizontalHeader()->setStretchLastSection(true);
  adock->setWidget(analyses_);
  addDockWidget(Qt::BottomDockWidgetArea, adock);
  connect(analyses_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (filling_ || item->column() != 3) return;
    const QString id = item->data(Qt::UserRole).toString();
    toggle_exclusion({id});
  });
  connect(analyses_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    if (auto* item = analyses_->item(row, 3)) emit recall_requested(item->data(Qt::UserRole).toString());
  });

  status_ = new QLabel;
  statusBar()->addWidget(status_, 1);

  debounce_.setSingleShot(true);
  debounce_.setInterval(150);
  connect(&debounce_, &QTimer::timeout, this, &FigureWindow::run);

  editor_->set_options(pipeline_.find(kFigure)->options);
  presets_->reload(QStringLiteral("Default"));
  run();
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
    if (!quantities_set_ && dataset_) {
      std::vector<pp::ReducedPtr> v;
      for (const auto& it : dataset_->items()) v.push_back(it.analysis);
      QStringList q;
      for (const auto& s : pp::available_quantities(v)) q << qs(s);
      editor_->set_quantity_choices(q);
      quantities_set_ = true;
    }
  }
  if (r.outputs.empty() || !r.outputs[0]) {
    const QString what = r.outputs.empty() ? tr("nothing computed") : qs(to_string(r.outputs[0].error()));
    status_->setText(tr("Error: %1").arg(what));
    view_->set_scene(nullptr);
    emit figure_updated();
    return;
  }
  auto scene = std::get<pp::ScenePtr>(r.outputs[0]->at(0));
  for (const auto& w : scene->warnings) warnings << qs(w);
  view_->set_scene(scene);
  const int n = dataset_ ? static_cast<int>(dataset_->size()) : 0;
  status_->setText(tr("%1 analyses · %2 s").arg(n).arg(r.seconds, 0, 'f', 2) +
                   (warnings.isEmpty() ? QString() : tr(" · %n warning(s)", nullptr, static_cast<int>(warnings.size()))));
  status_->setToolTip(warnings.join(QLatin1Char('\n')));
  emit figure_updated();
}

void FigureWindow::fill_analyses() {
  filling_ = true;
  analyses_->setRowCount(dataset_ ? static_cast<int>(dataset_->size()) : 0);
  if (dataset_) {
    int row = 0;
    for (const auto& it : dataset_->items()) {
      const auto& a = *it.analysis->analysis;
      analyses_->setItem(row, 0, new QTableWidgetItem(qs(a.runid)));
      analyses_->setItem(row, 1, new QTableWidgetItem(qs(a.analysis_type)));
      analyses_->setItem(row, 2, new QTableWidgetItem(qs(dataset_->group_name(it.path.group))));
      auto* inc = new QTableWidgetItem;
      inc->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      inc->setCheckState(it.exclusion.included() ? Qt::Checked : Qt::Unchecked);
      inc->setData(Qt::UserRole, qs(a.uuid));
      QString why;
      if (it.exclusion.user) why = *it.exclusion.user ? tr("excluded by you") : tr("included by you");
      else if (it.exclusion.filter) why = tr("filtered");
      else if (it.exclusion.tag) why = tr("tag: %1").arg(qs(a.tag));
      inc->setText(why);
      analyses_->setItem(row, 3, inc);
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

void FigureWindow::set_group_key(const QString& key) {
  if (group_->currentText() != key) {
    group_->setCurrentText(key);  // re-enters through currentTextChanged
    return;
  }
  (void)pipeline_.find("group")->options.set("key", key.toStdString());
  run();
}

QComboBox* FigureWindow::preset_combo() const noexcept { return presets_->combo(); }

void FigureWindow::select_preset(const QString& name) { presets_->select(name); }

bool FigureWindow::export_figure(const QString& path) {
  if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) return view_->save_pdf(path);
  return view_->save_png(path);
}

}  // namespace pychron::ui
