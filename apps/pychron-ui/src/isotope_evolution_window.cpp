#include "isotope_evolution_window.hpp"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "options_editor.hpp"
#include "preset_bar.hpp"
#include "pychron/processing/isotope_classifier.hpp"
#include "pychron/processing/recall.hpp"
#include "pychron/processing/revisions.hpp"
#include "scene_view.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::vector<std::string> to_std(const QStringList& l) {
  std::vector<std::string> out;
  for (const auto& s : l) out.push_back(s.toStdString());
  return out;
}

constexpr const char* kFit = "fit";
constexpr const char* kEdits = "edits";

QString flag_text(const pp::GoodnessFlag& f) {
  const char* op = f.check == "rsquared" ? "<=" : f.check == "outliers" || f.check == "slope" || f.check == "percent_error" ? ">" : ">=";
  return QStringLiteral("%1 %2 %3 %4 %5")
      .arg(qs(f.key), qs(f.check), QString::number(f.value, 'g', 4), QString::fromLatin1(op),
           QString::number(f.threshold, 'g', 4));
}

}  // namespace

IsotopeEvolutionWindow::IsotopeEvolutionWindow(ProcessingBridge& bridge, pp::PresetStore& presets, QStringList uuids,
                                               QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      store_(presets),
      schema_(pp::isotope_evolution_fit_schema()),
      channel_(bridge.new_channel()) {
  setWindowTitle(tr("Isotope evolutions — %n analyses", nullptr, static_cast<int>(uuids.size())));
  resize(1250, 860);

  const auto& reg = pp::UnitRegistry::builtin();
  auto& select = pipeline_.add(reg, "select", "select");
  (void)select.options.set("uuids", to_std(uuids));
  (void)select.options.set("remove_tags", std::vector<std::string>{});  // the user picked these
  pipeline_.add(reg, "reduce", "reduce", {"select"});
  pipeline_.add(reg, kEdits, "edits", {"reduce"});
  auto& fit = pipeline_.add(reg, kFit, kKind, {kEdits});
  fit.options = store_.defaults(schema_);
  fit.preset = "Default";

  auto* split = new QSplitter(Qt::Vertical);
  view_ = new SceneView;
  preview_ = new SceneView;
  auto* preview_host = new QWidget;
  auto* pl = new QVBoxLayout(preview_host);
  pl->setContentsMargins(0, 0, 0, 0);
  preview_kind_ = new QTabBar;
  preview_kind_->addTab(tr("Signals"));
  preview_kind_->addTab(tr("Baselines"));
  pl->addWidget(preview_kind_);
  pl->addWidget(preview_, 1);
  auto* train_row = new QHBoxLayout;
  train_row->addWidget(new QLabel(tr("Train the classifier:")));
  train_isotope_ = new QComboBox;
  train_row->addWidget(train_isotope_);
  train_good_ = new QPushButton(tr("Good"));
  train_bad_ = new QPushButton(tr("Bad"));
  train_row->addWidget(train_good_);
  train_row->addWidget(train_bad_);
  training_ = new QLabel;
  train_row->addWidget(training_, 1);
  pl->addLayout(train_row);
  connect(train_good_, &QPushButton::clicked, this, [this] { train(train_isotope_->currentText(), 1); });
  connect(train_bad_, &QPushButton::clicked, this, [this] { train(train_isotope_->currentText(), 0); });
  split->addWidget(view_);
  split->addWidget(preview_host);
  connect(preview_kind_, &QTabBar::currentChanged, this, [this] {
    if (!previewed_.isEmpty()) preview(previewed_);
  });
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 2);
  setCentralWidget(split);
  connect(view_, &SceneView::point_clicked, this, [this](const QString& id) { toggle_exclusion({id}); });
  connect(view_, &SceneView::points_toggled, this, [this](const QStringList& ids) { toggle_exclusion(ids); });

  auto* tools = addToolBar(tr("Isotope evolutions"));
  tools->addAction(tr("Reset view"), view_, &SceneView::reset_view);
  tools->addSeparator();
  only_good_ = new QCheckBox(tr("Leave out flagged when saving"));
  tools->addWidget(only_good_);
  connect(only_good_, &QCheckBox::toggled, this, [this] { update_save_state(); });
  save_ = new QPushButton(tr("Save"));
  tools->addWidget(save_);
  connect(save_, &QPushButton::clicked, this, [this] { save(); });

  auto* dock = new QDockWidget(tr("Fits"), this);
  auto* host = new QWidget;
  auto* hl = new QVBoxLayout(host);
  presets_ = new PresetBar(store_, schema_);
  presets_->current = [this] { return pipeline_.find(kFit)->options; };
  hl->addWidget(presets_);
  editor_ = new OptionsEditor;
  hl->addWidget(editor_, 1);
  dock->setWidget(host);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  connect(presets_, &PresetBar::loaded, this, [this](const pp::Options& o, const QString& name) {
    pipeline_.find(kFit)->preset = name.toStdString();
    set_fit_options(o);
  });
  connect(presets_, &PresetBar::message, this, [this](const QString& text, const QString& details) {
    status_->setText(text);
    if (!details.isEmpty()) status_->setToolTip(details);
  });
  connect(editor_, &OptionsEditor::changed, this, [this] {
    pipeline_.find(kFit)->options = editor_->options();
    note_.clear();
    debounce_.start();
  });

  auto* adock = new QDockWidget(tr("Analyses"), this);
  table_ = new QTableWidget;
  table_->setColumnCount(4);
  table_->setHorizontalHeaderLabels({tr("Run ID"), tr("Included"), tr("Flags"), tr("Refits")});
  table_->verticalHeader()->setVisible(false);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  adock->setWidget(table_);
  addDockWidget(Qt::BottomDockWidgetArea, adock);
  connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (filling_ || item->column() != 1) return;
    toggle_exclusion({item->data(Qt::UserRole).toString()});
  });
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    if (filling_) return;
    const auto rows = table_->selectionModel()->selectedRows();
    if (!rows.isEmpty()) preview(table_->item(rows.front().row(), 1)->data(Qt::UserRole).toString());
  });

  status_ = new QLabel;
  status_->setWordWrap(true);
  statusBar()->addWidget(status_, 1);

  debounce_.setSingleShot(true);
  debounce_.setInterval(150);
  connect(&debounce_, &QTimer::timeout, this, &IsotopeEvolutionWindow::run);

  set_classifier_file(std::filesystem::path(
                          QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) /
                      "classifier" / "isotope.toml");
  presets_->reload(QStringLiteral("Default"));
  update_save_state();
  run();
}

IsotopeEvolutionWindow::~IsotopeEvolutionWindow() { bridge_.release(channel_); }

void IsotopeEvolutionWindow::run() {
  debounce_.stop();
  status_->setText(tr("Refitting..."));
  bridge_.submit(channel_, pipeline_, {kFit, kEdits}, [this](const PipelineResult& r) { on_result(r); });
}

void IsotopeEvolutionWindow::on_result(const PipelineResult& r) {
  ++runs_;
  QStringList warnings;
  for (const auto& d : r.diagnostics) warnings << qs(d);
  if (r.outputs.size() >= 2 && r.outputs[1]) dataset_ = std::get<pp::DatasetPtr>(r.outputs[1]->at(0));
  if (r.outputs.empty() || !r.outputs[0]) {
    const QString what = r.outputs.empty() ? tr("nothing computed") : qs(to_string(r.outputs[0].error()));
    status_->setText(tr("Error: %1").arg(what));
    view_->set_scene(nullptr);
    fits_.reset();
    fill_table();
    update_save_state();
    emit figure_updated();
    return;
  }
  auto scene = std::get<pp::ScenePtr>(r.outputs[0]->at(0));
  fits_ = std::get<pp::IsotopeFitSetPtr>(r.outputs[0]->at(1));
  for (const auto& w : scene->warnings)
    if (!warnings.contains(qs(w))) warnings << qs(w);
  view_->set_scene(scene);
  fill_table();
  if (!previewed_.isEmpty() && !preview(previewed_)) preview_->set_scene(nullptr);
  status_->setText(tr("%n analyses refitted", nullptr, static_cast<int>(fits_->analyses.size())) +
                   (fits_->flagged() ? tr(", %1 flagged").arg(fits_->flagged()) : QString()) +
                   (warnings.isEmpty() ? QString() : tr(" · %n warning(s)", nullptr, static_cast<int>(warnings.size()))) +
                   (note_.isEmpty() ? QString() : QStringLiteral(" · ") + note_));
  status_->setToolTip(warnings.join(QLatin1Char('\n')));
  update_save_state();
  emit figure_updated();
}

const pp::AnalysisRefits* IsotopeEvolutionWindow::refits_of(const std::string& uuid) const {
  if (!fits_) return nullptr;
  for (const auto& a : fits_->analyses)
    if (a.uuid == uuid) return &a;
  return nullptr;
}

void IsotopeEvolutionWindow::fill_table() {
  filling_ = true;
  table_->setRowCount(dataset_ ? static_cast<int>(dataset_->size()) : 0);
  int row = 0;
  if (dataset_)
    for (const auto& it : dataset_->items()) {
      const auto& a = *it.analysis->analysis;
      table_->setItem(row, 0, new QTableWidgetItem(qs(a.runid)));
      auto* inc = new QTableWidgetItem;
      inc->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      inc->setCheckState(it.exclusion.included() ? Qt::Checked : Qt::Unchecked);
      inc->setData(Qt::UserRole, qs(a.uuid));
      table_->setItem(row, 1, inc);
      QStringList flags, refits;
      if (const auto* r = refits_of(a.uuid)) {
        for (const auto& f : r->flags) flags << flag_text(f);
        for (const auto& i : r->isotopes) {
          const double v = i.fit.value.value;
          refits << QStringLiteral("%1 %2 ± %3%").arg(qs(i.label()), QString::number(v, 'g', 7),
                                                       QString::number(v != 0 ? std::abs(i.fit.value.error / v) * 100 : 0, 'f', 3));
        }
      } else {
        refits << (it.exclusion.excluded() ? tr("left out") : tr("not refitted"));
      }
      auto* flag_item = new QTableWidgetItem(flags.join(QStringLiteral("; ")));
      if (!flags.isEmpty()) flag_item->setBackground(theme().warning_bg);
      table_->setItem(row, 2, flag_item);
      table_->setItem(row, 3, new QTableWidgetItem(refits.join(QStringLiteral("   "))));
      ++row;
    }
  table_->resizeColumnsToContents();
  filling_ = false;
}

bool IsotopeEvolutionWindow::preview(const QString& uuid) {
  previewed_ = uuid;
  const auto* r = refits_of(uuid.toStdString());
  {
    const QString keep = train_isotope_->currentText();
    const QSignalBlocker block(train_isotope_);
    train_isotope_->clear();
    if (r)
      for (const auto& iso : r->edited ? r->edited->isotopes : std::vector<pp::IsotopeData>{})
        train_isotope_->addItem(qs(iso.key));
    const int at = train_isotope_->findText(keep);
    train_isotope_->setCurrentIndex(at >= 0 ? at : 0);
  }
  update_training_state();
  if (!r || !r->edited) {
    preview_->set_scene(nullptr);
    return false;
  }
  auto raw = bridge_.source().load_raw(uuid.toStdString());
  if (!raw) {
    status_->setText(tr("Preview: %1").arg(qs(raw.error().what)));
    preview_->set_scene(nullptr);
    return false;
  }
  const auto kind = preview_kind_->currentIndex() == 1 ? pp::SeriesKind::Baseline : pp::SeriesKind::Signal;
  preview_->set_scene(std::make_shared<const pp::Scene>(pp::make_evolution_scene(*r->edited, *raw, kind)));
  return true;
}

void IsotopeEvolutionWindow::toggle_exclusion(const QStringList& uuids) {
  auto& o = pipeline_.find(kEdits)->options;
  auto ex = o.get_strings("exclude"), inc = o.get_strings("include");
  for (const auto& qid : uuids) {
    const std::string id = qid.toStdString();
    bool known = false, excluded_now = false, tag_or_filter = false;
    if (dataset_)
      for (const auto& it : dataset_->items())
        if (it.analysis->analysis->uuid == id) {
          known = true;
          excluded_now = it.exclusion.excluded();
          tag_or_filter = it.exclusion.tag || it.exclusion.filter;
        }
    if (!known) continue;
    ex.erase(std::remove(ex.begin(), ex.end(), id), ex.end());
    inc.erase(std::remove(inc.begin(), inc.end(), id), inc.end());
    if (excluded_now) {
      if (tag_or_filter) inc.push_back(id);
    } else {
      ex.push_back(id);
    }
  }
  (void)o.set("exclude", ex);
  (void)o.set("include", inc);
  note_.clear();
  run();
}

void IsotopeEvolutionWindow::set_fit_options(const pp::Options& options) {
  pipeline_.find(kFit)->options = options;
  editor_->set_options(options);
  note_.clear();
  run();
}

void IsotopeEvolutionWindow::update_save_state() {
  pp::IRevisionSource* revisions = bridge_.source().revisions();
  int n = 0;
  if (fits_)
    for (const auto& a : fits_->analyses) n += !only_good_->isChecked() || a.good();
  save_->setEnabled(revisions && n > 0);
  if (!revisions) {
    save_->setToolTip(tr("Saving needs a source that keeps revisions (pychron-ui --db)"));
  } else if (n == 0) {
    save_->setToolTip(tr("Nothing refitted to save"));
  } else {
    save_->setToolTip(tr("Save the refitted intercepts of %n analyses in one changeset", nullptr, n));
  }
}

bool IsotopeEvolutionWindow::save() {
  update_save_state();
  pp::IRevisionSource* revisions = bridge_.source().revisions();
  if (!revisions || !fits_ || !save_->isEnabled()) {
    status_->setText(save_->toolTip());
    return false;
  }
  pp::IsotopeFitSet chosen = *fits_;
  if (only_good_->isChecked())
    chosen.analyses.erase(std::remove_if(chosen.analyses.begin(), chosen.analyses.end(),
                                         [](const pp::AnalysisRefits& a) { return !a.good(); }),
                          chosen.analyses.end());
  auto outcome = revisions->save_isotope_fits(chosen);
  if (!outcome) {
    status_->setText(tr("Save failed: %1").arg(qs(outcome.error().what)));
    return false;
  }
  if (!outcome->saved) {
    status_->setText(tr("Not saved: %1").arg(qs(outcome->conflict)));
    return false;
  }
  QStringList ids;
  for (const auto& a : chosen.analyses) ids << qs(a.uuid);
  note_ = tr("saved intercepts of %n analyses", nullptr, static_cast<int>(ids.size()));
  status_->setText(note_);
  run();
  emit saved(ids);
  return true;
}

void IsotopeEvolutionWindow::set_classifier_file(const std::filesystem::path& file) {
  classifier_file_ = file;
  auto& o = pipeline_.find(kFit)->options;
  (void)o.set("classifier_file", file.string());
  editor_->set_options(o);
  update_training_state();
}

void IsotopeEvolutionWindow::update_training_state() {
  auto loaded = pp::IsotopeClassifier::load(classifier_file_);
  int good = 0, bad = 0;
  if (loaded)
    for (const auto& s : loaded->samples()) (s.klass ? good : bad) += 1;
  training_->setText(loaded ? tr("%1 good, %2 bad").arg(good).arg(bad) : tr("training file unreadable"));
  training_->setToolTip(qs(classifier_file_.string()));
  const bool can = !previewed_.isEmpty() && train_isotope_->count() > 0 && !classifier_file_.empty();
  train_good_->setEnabled(can);
  train_bad_->setEnabled(can);
}

bool IsotopeEvolutionWindow::train(const QString& key, int klass) {
  const auto* r = refits_of(previewed_.toStdString());
  const pp::IsotopeData* iso = r && r->edited ? r->edited->find_isotope(key.toStdString()) : nullptr;
  if (!iso || classifier_file_.empty()) {
    status_->setText(tr("Train: preview an analysis and pick one of its isotopes first"));
    return false;
  }
  auto raw = bridge_.source().load_raw(previewed_.toStdString());
  if (!raw) {
    status_->setText(tr("Train: %1").arg(qs(raw.error().what)));
    return false;
  }
  auto classifier = pp::IsotopeClassifier::load(classifier_file_);
  if (!classifier) {
    status_->setText(tr("Train: %1").arg(qs(classifier.error().what)));
    return false;
  }
  auto sample = pp::make_isotope_sample(*raw, iso->key, iso->isotope);
  sample.klass = klass;
  sample.runid = r->runid;
  classifier->add(std::move(sample));
  if (auto ok = classifier->save(classifier_file_); !ok) {
    status_->setText(tr("Train: %1").arg(qs(ok.error().what)));
    return false;
  }
  // A new stamp reruns the refits when the classifier is in use.
  auto& o = pipeline_.find(kFit)->options;
  (void)o.set("classifier_stamp", std::to_string(classifier->samples().size()) + "." +
                                      std::to_string(++training_version_));
  editor_->set_options(o);
  note_ = tr("%1 of %2 marked %3").arg(key, qs(r->runid), klass ? tr("good") : tr("bad"));
  update_training_state();
  run();
  return true;
}

}  // namespace pychron::ui
