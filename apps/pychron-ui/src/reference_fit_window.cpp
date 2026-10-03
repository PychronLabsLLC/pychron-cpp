#include "reference_fit_window.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "options_editor.hpp"
#include "pychron/processing/revisions.hpp"
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

constexpr const char* kFit = "fit";
constexpr const char* kReferences = "references";
constexpr const char* kEdits = "reference_edits";

}  // namespace

ReferenceFitWindow::ReferenceFitWindow(ProcessingBridge& bridge, pp::PresetStore& presets, std::string kind,
                                       QStringList unknowns, QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      store_(presets),
      kind_(std::move(kind)),
      target_(kind_ == "icfactor_fit" ? pp::ReferenceFitTarget::IcFactors : pp::ReferenceFitTarget::Blanks),
      schema_(pp::UnitRegistry::builtin().find(kind_)->schema()),
      unknowns_(std::move(unknowns)),
      channel_(bridge.new_channel()) {
  const bool blanks = target_ == pp::ReferenceFitTarget::Blanks;
  setWindowTitle(tr("%1 — %n unknowns", nullptr, static_cast<int>(unknowns_.size()))
                     .arg(blanks ? tr("Blanks") : tr("IC factors")));
  resize(1200, 820);

  const auto& reg = pp::UnitRegistry::builtin();
  auto& u = pipeline_.add(reg, "unknowns", "select");
  (void)u.options.set("uuids", to_std(unknowns_));
  (void)u.options.set("remove_tags", std::vector<std::string>{});  // the user picked these
  pipeline_.add(reg, "reduce_unknowns", "reduce", {"unknowns"});
  // No uuids yet: a type no analysis has, so the select is empty rather than
  // the whole source.
  auto& refs = pipeline_.add(reg, kReferences, "select");
  (void)refs.options.set("analysis_types", std::vector<std::string>{"-"});
  pipeline_.add(reg, "reduce_references", "reduce", {kReferences});
  pipeline_.add(reg, kEdits, "edits", {"reduce_references"});
  auto& fit = pipeline_.add(reg, kFit, kind_, {"reduce_unknowns", kEdits});
  fit.options = store_.defaults(schema_);
  fit.preset = "Default";

  view_ = new SceneView;
  setCentralWidget(view_);
  connect(view_, &SceneView::point_clicked, this, [this](const QString& id) { toggle_references({id}); });
  connect(view_, &SceneView::points_toggled, this, [this](const QStringList& ids) { toggle_references(ids); });

  // Finder.
  auto* finder = addToolBar(tr("References"));
  finder->addWidget(new QLabel(tr(" References of type ")));
  types_ = new QLineEdit;
  QStringList types;
  for (const auto& t : pp::default_reference_types(target_)) types << qs(t);
  types_->setText(types.join(QStringLiteral(", ")));
  types_->setMinimumWidth(260);
  types_->setToolTip(tr("Analysis types, comma separated"));
  finder->addWidget(types_);
  finder->addWidget(new QLabel(tr(" within ")));
  hours_ = new QDoubleSpinBox;
  hours_->setRange(0.5, 24.0 * 365);
  hours_->setDecimals(1);
  hours_->setValue(10.0);
  hours_->setSuffix(tr(" h"));
  finder->addWidget(hours_);
  same_ms_ = new QCheckBox(tr("same spectrometer"));
  same_ms_->setChecked(true);
  same_device_ = new QCheckBox(tr("same extract device"));
  finder->addWidget(same_ms_);
  finder->addWidget(same_device_);
  auto* find = new QPushButton(tr("Find references"));
  finder->addWidget(find);
  connect(find, &QPushButton::clicked, this, [this] { find_references(); });
  finder->addSeparator();
  finder->addAction(tr("Reset view"), view_, &SceneView::reset_view);
  save_ = new QPushButton(tr("Save"));
  finder->addWidget(save_);
  connect(save_, &QPushButton::clicked, this, [this] { save(); });

  auto* dock = new QDockWidget(tr("Fits"), this);
  editor_ = new OptionsEditor;
  dock->setWidget(editor_);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  connect(editor_, &OptionsEditor::changed, this, [this] {
    pipeline_.find(kFit)->options = editor_->options();
    note_.clear();
    debounce_.start();
  });

  status_ = new QLabel;
  status_->setWordWrap(true);
  statusBar()->addWidget(status_, 1);

  debounce_.setSingleShot(true);
  debounce_.setInterval(150);
  connect(&debounce_, &QTimer::timeout, this, &ReferenceFitWindow::run);

  editor_->set_options(pipeline_.find(kFit)->options);
  update_save_state();
  if (!find_references()) run();
}

ReferenceFitWindow::~ReferenceFitWindow() { bridge_.release(channel_); }

QStringList ReferenceFitWindow::reference_uuids() const {
  QStringList out;
  for (const auto& id : pipeline_.find(kReferences)->options.get_strings("uuids")) out << qs(id);
  return out;
}

bool ReferenceFitWindow::find_references() {
  std::vector<pp::AnalysisPtr> unknowns;
  for (const auto& id : unknowns_)
    if (auto a = bridge_.source().load(id.toStdString())) unknowns.push_back(*a);
  pp::ReferenceQuery q;
  for (auto t : types_->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    t = t.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
    if (!t.isEmpty()) q.analysis_types.push_back(t.toStdString());
  }
  q.hours = hours_->value();
  q.same_mass_spectrometer = same_ms_->isChecked();
  q.same_extract_device = same_device_->isChecked();
  auto found = pp::find_references(bridge_.source(), unknowns, q);
  if (!found) {
    status_->setText(tr("Finding references failed: %1").arg(qs(found.error().what)));
    return false;
  }
  auto& refs = pipeline_.find(kReferences)->options;
  (void)refs.set("uuids", *found);
  // An empty selection selects nothing rather than everything.
  (void)refs.set("analysis_types", found->empty() ? std::vector<std::string>{"-"} : std::vector<std::string>{});
  note_.clear();
  (void)pipeline_.find(kEdits)->options.set("exclude", std::vector<std::string>{});
  (void)pipeline_.find(kEdits)->options.set("include", std::vector<std::string>{});
  run();
  return true;
}

void ReferenceFitWindow::run() {
  debounce_.stop();
  status_->setText(tr("Computing..."));
  bridge_.submit(channel_, pipeline_, {kFit, kEdits}, [this](const PipelineResult& r) { on_result(r); });
}

void ReferenceFitWindow::on_result(const PipelineResult& r) {
  ++runs_;
  QStringList warnings;
  for (const auto& d : r.diagnostics) warnings << qs(d);
  if (r.outputs.size() >= 2 && r.outputs[1]) references_ = std::get<pp::DatasetPtr>(r.outputs[1]->at(0));
  if (r.outputs.empty() || !r.outputs[0]) {
    const QString what = r.outputs.empty() ? tr("nothing computed") : qs(to_string(r.outputs[0].error()));
    status_->setText(tr("Error: %1").arg(what));
    view_->set_scene(nullptr);
    fits_.reset();
    update_save_state();
    emit figure_updated();
    return;
  }
  auto scene = std::get<pp::ScenePtr>(r.outputs[0]->at(0));
  fits_ = std::get<pp::ReferenceFitSetPtr>(r.outputs[0]->at(1));
  for (const auto& w : scene->warnings)
    if (!warnings.contains(qs(w))) warnings << qs(w);
  view_->set_scene(scene);
  const int nrefs = references_ ? static_cast<int>(references_->size()) : 0;
  int excluded = 0;
  if (references_)
    for (const auto& it : references_->items()) excluded += it.exclusion.excluded();
  status_->setText(tr("%n reference(s)", nullptr, nrefs) +
                   (excluded ? tr(", %1 left out").arg(excluded) : QString()) +
                   tr(" · %n unknown(s) fitted", nullptr, static_cast<int>(fits_->analyses.size())) +
                   (warnings.isEmpty() ? QString() : tr(" · %n warning(s)", nullptr, static_cast<int>(warnings.size()))) +
                   (note_.isEmpty() ? QString() : QStringLiteral(" · ") + note_));
  status_->setToolTip(warnings.join(QLatin1Char('\n')));
  update_save_state();
  emit figure_updated();
}

void ReferenceFitWindow::toggle_references(const QStringList& uuids) {
  auto& o = pipeline_.find(kEdits)->options;
  auto ex = o.get_strings("exclude"), inc = o.get_strings("include");
  for (const auto& qid : uuids) {
    const std::string id = qid.toStdString();
    bool known = false, excluded_now = false, tag_or_filter = false;
    if (references_)
      for (const auto& it : references_->items())
        if (it.analysis->analysis->uuid == id) {
          known = true;
          excluded_now = it.exclusion.excluded();
          tag_or_filter = it.exclusion.tag || it.exclusion.filter;
        }
    if (!known) continue;  // an unknown's point
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

void ReferenceFitWindow::set_fit_options(const pp::Options& options) {
  pipeline_.find(kFit)->options = options;
  editor_->set_options(options);
  run();
}

void ReferenceFitWindow::update_save_state() {
  pp::IRevisionSource* revisions = bridge_.source().revisions();
  const bool any = fits_ && !fits_->analyses.empty();
  save_->setEnabled(revisions && any);
  if (!revisions) {
    save_->setToolTip(tr("Saving needs a source that keeps revisions (pychron-ui --db)"));
  } else if (!any) {
    save_->setToolTip(tr("Nothing fitted to save"));
  } else {
    save_->setToolTip(tr("Save the predicted %1 of %n unknown(s) in one changeset", nullptr,
                         static_cast<int>(fits_->analyses.size()))
                          .arg(target_ == pp::ReferenceFitTarget::Blanks ? tr("blanks") : tr("IC factors")));
  }
}

bool ReferenceFitWindow::save() {
  pp::IRevisionSource* revisions = bridge_.source().revisions();
  if (!revisions || !fits_ || fits_->analyses.empty()) {
    status_->setText(save_->toolTip());
    return false;
  }
  auto outcome = revisions->save_reference_fits(*fits_);
  if (!outcome) {
    status_->setText(tr("Save failed: %1").arg(qs(outcome.error().what)));
    return false;
  }
  if (!outcome->saved) {
    status_->setText(tr("Not saved: %1").arg(qs(outcome->conflict)));
    return false;
  }
  QStringList ids;
  for (const auto& a : fits_->analyses) ids << qs(a.uuid);
  note_ = tr("saved %1 for %n analyses", nullptr, static_cast<int>(ids.size()))
              .arg(target_ == pp::ReferenceFitTarget::Blanks ? tr("blanks") : tr("IC factors"));
  status_->setText(note_);
  run();  // the source's generation changed: reload with the new heads
  emit saved(ids);
  return true;
}

}  // namespace pychron::ui
