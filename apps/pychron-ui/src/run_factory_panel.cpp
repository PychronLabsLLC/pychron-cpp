#include "run_factory_panel.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "pychron/experiment/factory/blocks.hpp"
#include "pychron/experiment/model/positions.hpp"

namespace pychron::ui {

namespace {

using experiment::AnalysisType;
using experiment::FactoryForm;
using experiment::Unit;
using scripting::ScriptKind;

constexpr std::array<Unit, 5> kUnits = {Unit::Watts, Unit::Percent, Unit::Temp, Unit::Amps, Unit::Volts};
// Types offered in the type combo, unknown first; specials get their prefix.
constexpr std::array<AnalysisType, 10> kTypes = {
    AnalysisType::Unknown, AnalysisType::BlankUnknown,        AnalysisType::BlankAir, AnalysisType::BlankCocktail,
    AnalysisType::Air,     AnalysisType::Cocktail,            AnalysisType::Background,
    AnalysisType::BlankExtractionLine, AnalysisType::Degas, AnalysisType::DetectorIC};

QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string s(const QString& t) { return t.trimmed().toStdString(); }

QComboBox* name_combo(const std::vector<std::string>& names, bool allow_empty) {
  auto* c = new QComboBox;
  c->setEditable(true);
  c->setInsertPolicy(QComboBox::NoInsert);
  if (allow_empty) c->addItem(QString());
  for (const auto& n : names) c->addItem(q(n));
  return c;
}

void set_combo_text(QComboBox* c, const std::string& text) {
  const QString t = q(text);
  const int i = c->findText(t);
  if (i >= 0) c->setCurrentIndex(i);
  else c->setEditText(t);
}

QDoubleSpinBox* seconds_box() {
  auto* b = new QDoubleSpinBox;
  b->setRange(0, 24 * 3600);
  b->setDecimals(1);
  b->setSuffix(QStringLiteral(" s"));
  return b;
}

}  // namespace

RunFactoryPanel::RunFactoryPanel(const experiment::lab::Lab& lab, QueueTableModel& model, Selection selection,
                                 QWidget* parent)
    : QWidget(parent), lab_(lab), model_(model), selection_(std::move(selection)) {
  auto* inner = new QWidget;
  auto* column = new QVBoxLayout(inner);
  for (QWidget* g : {build_run(), build_extraction(), build_measurement(), build_frequency(), build_block()}) {
    column->addWidget(g);
    groups_.push_back(g);
  }
  column->addStretch(1);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // the form narrows; only scroll down
  scroll->setWidget(inner);
  // The preview and Add stay in view below the scrolling form.
  QWidget* add_box = build_add();
  groups_.push_back(add_box);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll, 1);
  outer->addWidget(add_box);

  // Start from the lab's defaults for an unknown, as a new pychron factory does.
  FactoryForm start;
  if (auto d = experiment::with_lab_defaults(start, lab_.ids, lab_.defaults)) start = *d;
  set_form(start);
  connect(&model_, &QueueTableModel::validated, this, [this] { refresh(); });
}

QWidget* RunFactoryPanel::build_run() {
  auto* box = new QGroupBox(tr("Run"));
  auto* form = new QFormLayout(box);
  type_ = new QComboBox;
  for (auto t : kTypes) type_->addItem(q(experiment::to_string(t)), static_cast<int>(t));
  package_ = new QComboBox;
  package_->setObjectName(QStringLiteral("package"));
  package_->addItem(tr("(none)"));
  level_ = new QComboBox;
  level_->setObjectName(QStringLiteral("level"));
  level_->addItem(tr("(all)"));
  level_->setEnabled(false);
  identifier_select_ = new QComboBox;
  identifier_select_->setObjectName(QStringLiteral("identifier_select"));
  identifier_select_->setEditable(true);
  identifier_select_->setInsertPolicy(QComboBox::NoInsert);
  identifier_select_->setCompleter(nullptr);
  identifier_ = identifier_select_->lineEdit();
  identifier_->setObjectName(QStringLiteral("identifier"));
  identifier_->setPlaceholderText(tr("e.g. 66001, or a special such as bu"));
  aliquot_ = new QLineEdit;
  aliquot_->setPlaceholderText(tr("auto"));
  aliquot_->setValidator(new QIntValidator(1, 1'000'000, aliquot_));
  step_ = new QLineEdit;
  run_form_ = form;
  form->addRow(tr("Type"), type_);
  form->addRow(tr("Package"), package_);
  form->addRow(tr("Level"), level_);
  form->addRow(tr("Identifier"), identifier_select_);
  form->addRow(tr("Aliquot"), aliquot_);
  form->addRow(tr("Step"), step_);

  connect(type_, &QComboBox::activated, this, [this](int) {
    const auto t = static_cast<AnalysisType>(type_->currentData().toInt());
    const std::string prefix = lab_.ids.prefix_for(t);
    if (t == AnalysisType::Unknown) {
      if (lab_.ids.is_special(s(identifier_->text()))) identifier_->clear();
    } else if (!prefix.empty()) {
      identifier_->setText(q(prefix));
    }
    on_identifier_changed();
  });
  connect(identifier_, &QLineEdit::textEdited, this, [this] { on_identifier_changed(); });
  connect(package_, &QComboBox::activated, this, [this](int) { load_contents(); });
  connect(level_, &QComboBox::activated, this, [this](int) { fill_identifiers(); });
  connect(identifier_select_, &QComboBox::activated, this, [this](int index) { choose_identifier(index); });
  show_selects(false);
  for (auto* e : {aliquot_, step_}) connect(e, &QLineEdit::textEdited, this, [this] { refresh(); });
  return box;
}

QWidget* RunFactoryPanel::build_extraction() {
  auto* box = new QGroupBox(tr("Extraction"));
  auto* form = new QFormLayout(box);
  device_ = new QLineEdit;
  device_->setObjectName(QStringLiteral("device"));
  position_ = new QLineEdit;
  position_->setObjectName(QStringLiteral("position"));
  position_->setPlaceholderText(tr("e.g. 4, 1-6, 1,3,5"));
  per_hole_ = new QCheckBox(tr("One run per hole"));
  per_hole_->setChecked(true);
  identifier_step_ = new QSpinBox;
  identifier_step_->setRange(0, 1000);
  identifier_step_->setToolTip(tr("With one run per hole: advance the identifier this much per run"));
  value_ = new QDoubleSpinBox;
  value_->setObjectName(QStringLiteral("value"));
  value_->setRange(0, 1e6);
  value_->setDecimals(3);
  units_ = new QComboBox;
  units_->setObjectName(QStringLiteral("units"));
  for (auto u : kUnits) units_->addItem(q(experiment::to_string(u)), static_cast<int>(u));
  auto* value_row = new QHBoxLayout;
  value_row->addWidget(value_, 1);
  value_row->addWidget(units_);
  duration_ = seconds_box();
  duration_->setObjectName(QStringLiteral("duration"));
  cleanup_ = seconds_box();
  cleanup_->setObjectName(QStringLiteral("cleanup"));
  script_ = name_combo(lab_.scripts->names(ScriptKind::Extraction), true);
  script_->setObjectName(QStringLiteral("script"));
  step_heat_ = new QLineEdit;
  step_heat_->setObjectName(QStringLiteral("step_heat"));
  step_heat_->setPlaceholderText(tr("e.g. 5, 10, 15 or 5:2.5:4"));
  step_heat_->setToolTip(tr("One run per value with steps A, B, C ...; start:increment:count also works"));
  auto* hole_row = new QHBoxLayout;
  hole_row->addWidget(per_hole_);
  hole_row->addWidget(new QLabel(tr("id step")));
  hole_row->addWidget(identifier_step_);
  form->addRow(tr("Device"), device_);
  form->addRow(tr("Position"), position_);
  form->addRow(QString(), hole_row);
  form->addRow(tr("Value"), value_row);
  form->addRow(tr("Duration"), duration_);
  form->addRow(tr("Cleanup"), cleanup_);
  form->addRow(tr("Script"), script_);
  form->addRow(tr("Step heat"), step_heat_);

  for (auto* e : {device_, position_, step_heat_}) connect(e, &QLineEdit::textEdited, this, [this] { refresh(); });
  connect(per_hole_, &QCheckBox::toggled, this, [this] { refresh(); });
  connect(identifier_step_, &QSpinBox::valueChanged, this, [this] { refresh(); });
  for (auto* b : {value_, duration_, cleanup_}) connect(b, &QDoubleSpinBox::valueChanged, this, [this] { refresh(); });
  connect(units_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
  connect(script_, &QComboBox::currentTextChanged, this, [this] { refresh(); });
  return box;
}

QWidget* RunFactoryPanel::build_measurement() {
  auto* box = new QGroupBox(tr("Measurement"));
  auto* form = new QFormLayout(box);
  plan_ = name_combo(lab_.plans->names(), true);
  plan_->setObjectName(QStringLiteral("plan"));
  post_equilibration_ = name_combo(lab_.scripts->names(ScriptKind::PostEquilibration), true);
  post_equilibration_->setObjectName(QStringLiteral("post_equilibration"));
  post_measurement_ = name_combo(lab_.scripts->names(ScriptKind::PostMeasurement), true);
  post_measurement_->setObjectName(QStringLiteral("post_measurement"));
  comment_ = new QLineEdit;
  form->addRow(tr("Plan"), plan_);
  form->addRow(tr("Post-equilibration"), post_equilibration_);
  form->addRow(tr("Post-measurement"), post_measurement_);
  conditionals_button_ = new QToolButton;
  conditionals_button_->setObjectName(QStringLiteral("conditionals"));
  conditionals_button_->setPopupMode(QToolButton::InstantPopup);
  conditionals_button_->setMenu(new QMenu(conditionals_button_));
  conditionals_button_->setToolTip(tr("Conditionals files every added run gets"));
  form->addRow(tr("Conditionals"), conditionals_button_);
  form->addRow(tr("Comment"), comment_);
  refresh_conditionals();
  for (auto* c : {plan_, post_equilibration_, post_measurement_})
    connect(c, &QComboBox::currentTextChanged, this, [this] { refresh(); });
  connect(comment_, &QLineEdit::textEdited, this, [this] { refresh(); });
  return box;
}

QWidget* RunFactoryPanel::build_add() {
  auto* box = new QGroupBox(tr("Add"));
  auto* col = new QVBoxLayout(box);
  preview_ = new QLabel;
  preview_->setWordWrap(true);
  add_ = new QPushButton(tr("Add"));
  add_->setShortcut(key(Shortcut::AddRuns));
  add_->setToolTip(tr("Add the runs (%1)").arg(key(Shortcut::AddRuns).toString(QKeySequence::NativeText)));
  auto* defaults = new QPushButton(tr("Defaults"));
  defaults->setToolTip(tr("Apply the lab's defaults.toml entry for this type and device"));
  auto* from_row = new QPushButton(tr("From Row"));
  from_row->setToolTip(tr("Fill the form from the first selected queue row"));
  auto* buttons = new QHBoxLayout;
  buttons->addWidget(add_);
  buttons->addWidget(defaults);
  buttons->addWidget(from_row);
  after_selection_ = new QCheckBox(tr("Insert after the selected rows"));
  after_selection_->setChecked(true);
  inc_identifier_ = new QSpinBox;
  inc_identifier_->setRange(0, 1000);
  inc_identifier_->setToolTip(tr("After Add, advance the identifier this much"));
  inc_position_ = new QSpinBox;
  inc_position_->setRange(0, 1000);
  inc_position_->setToolTip(tr("After Add, advance the position this much past the last hole added"));
  auto* inc = new QHBoxLayout;
  inc->addWidget(new QLabel(tr("Then advance id")));
  inc->addWidget(inc_identifier_);
  inc->addWidget(new QLabel(tr("pos")));
  inc->addWidget(inc_position_);
  col->addWidget(preview_);
  col->addLayout(buttons);
  col->addWidget(after_selection_);
  col->addLayout(inc);
  connect(add_, &QPushButton::clicked, this, [this] { add(); });
  connect(defaults, &QPushButton::clicked, this, [this] { apply_defaults(); });
  connect(from_row, &QPushButton::clicked, this, [this] { load_from_selection(); });
  return box;
}

QWidget* RunFactoryPanel::build_frequency() {
  auto* box = new QGroupBox(tr("Frequency"));
  auto* form = new QFormLayout(box);
  freq_type_ = new QComboBox;
  for (auto t : kTypes)
    if (t != AnalysisType::Unknown && !lab_.ids.prefix_for(t).empty())
      freq_type_->addItem(q(experiment::to_string(t)), static_cast<int>(t));
  freq_every_ = new QSpinBox;
  freq_every_->setRange(0, 1000);
  freq_every_->setValue(3);
  freq_every_->setToolTip(tr("After every N unknowns; 0: only before/after"));
  freq_before_ = new QCheckBox(tr("Before"));
  freq_after_ = new QCheckBox(tr("After"));
  auto* edges = new QHBoxLayout;
  edges->addWidget(freq_before_);
  edges->addWidget(freq_after_);
  auto* insert = new QPushButton(tr("Insert"));
  insert->setToolTip(tr("Insert the special run among the unknowns (selected rows only, if any)"));
  form->addRow(tr("Type"), freq_type_);
  form->addRow(tr("Every"), freq_every_);
  form->addRow(QString(), edges);
  form->addRow(QString(), insert);
  connect(insert, &QPushButton::clicked, this, [this] { insert_frequency(); });
  return box;
}

QWidget* RunFactoryPanel::build_block() {
  auto* box = new QGroupBox(tr("Block"));
  block_box_ = box;
  auto* form = new QFormLayout(box);
  block_ = new QComboBox;
  for (const auto& [name, b] : lab_.blocks) {
    block_->addItem(q(name));
    block_->setItemData(block_->count() - 1, q(b.description), Qt::ToolTipRole);
  }
  block_times_ = new QSpinBox;
  block_times_->setRange(1, 100);
  auto* insert = new QPushButton(tr("Insert"));
  form->addRow(tr("Block"), block_);
  form->addRow(tr("Times"), block_times_);
  form->addRow(QString(), insert);
  connect(insert, &QPushButton::clicked, this, [this] { insert_block(); });
  if (lab_.blocks.empty()) {
    box->setToolTip(tr("No blocks in %1").arg(q((lab_.paths.dir / "blocks").string())));
    box->setEnabled(false);
  }
  return box;
}

FactoryForm RunFactoryPanel::form() const {
  FactoryForm f;
  f.identifier = s(identifier_->text());
  if (!aliquot_->text().trimmed().isEmpty()) f.aliquot = aliquot_->text().trimmed().toInt();
  f.step = s(step_->text());
  f.extract_device = s(device_->text());
  f.position = s(position_->text());
  f.one_run_per_hole = per_hole_->isChecked();
  f.identifier_step = identifier_step_->value();
  f.value = value_->value();
  f.units = static_cast<Unit>(units_->currentData().toInt());
  f.duration_s = duration_->value();
  f.cleanup_s = cleanup_->value();
  f.step_heat = s(step_heat_->text());
  f.script = s(script_->currentText());
  f.plan = s(plan_->currentText());
  f.post_equilibration = s(post_equilibration_->currentText());
  f.post_measurement = s(post_measurement_->currentText());
  f.comment = comment_->text().toStdString();
  f.overrides = overrides_carrier_.overrides;
  f.conditionals = conditionals_;
  return f;
}

void RunFactoryPanel::set_form(const FactoryForm& f) {
  updating_ = true;
  identifier_->setText(q(f.identifier));
  aliquot_->setText(f.aliquot ? QString::number(*f.aliquot) : QString());
  step_->setText(q(f.step));
  device_->setText(q(f.extract_device));
  position_->setText(q(f.position));
  per_hole_->setChecked(f.one_run_per_hole);
  identifier_step_->setValue(f.identifier_step);
  value_->setValue(f.value);
  units_->setCurrentIndex(std::max(0, units_->findData(static_cast<int>(f.units))));
  duration_->setValue(f.duration_s);
  cleanup_->setValue(f.cleanup_s);
  step_heat_->setText(q(f.step_heat));
  set_combo_text(script_, f.script);
  set_combo_text(plan_, f.plan);
  set_combo_text(post_equilibration_, f.post_equilibration);
  set_combo_text(post_measurement_, f.post_measurement);
  comment_->setText(q(f.comment));
  overrides_carrier_.overrides = f.overrides;
  conditionals_ = f.conditionals;
  refresh_conditionals();
  last_type_ = lab_.ids.classify(f.identifier);
  updating_ = false;
  refresh();
}

void RunFactoryPanel::on_identifier_changed() {
  if (updating_) return;
  const AnalysisType type = lab_.ids.classify(s(identifier_->text()));
  if (type != last_type_) {
    last_type_ = type;
    // A new type starts from its defaults, as in pychron's factory.
    if (auto d = experiment::with_lab_defaults(form(), lab_.ids, lab_.defaults)) {
      set_form(*d);
      return;
    }
  }
  refresh();
}

void RunFactoryPanel::refresh() {
  if (updating_) return;
  const FactoryForm f = form();
  const AnalysisType type = lab_.ids.classify(f.identifier);
  {
    const QSignalBlocker block(type_);
    type_->setCurrentIndex(std::max(0, type_->findData(static_cast<int>(type))));
  }
  const auto rules = experiment::form_rules(f, lab_.ids);
  for (QWidget* w : {static_cast<QWidget*>(device_), static_cast<QWidget*>(duration_), static_cast<QWidget*>(cleanup_),
                     static_cast<QWidget*>(script_)})
    w->setEnabled(rules.extraction);
  for (QWidget* w : {static_cast<QWidget*>(value_), static_cast<QWidget*>(units_), static_cast<QWidget*>(step_heat_)})
    w->setEnabled(rules.heating);
  for (QWidget* w : {static_cast<QWidget*>(position_), static_cast<QWidget*>(per_hole_),
                     static_cast<QWidget*>(identifier_step_)})
    w->setEnabled(rules.position);
  for (QWidget* w : {static_cast<QWidget*>(plan_), static_cast<QWidget*>(post_equilibration_),
                     static_cast<QWidget*>(post_measurement_)})
    w->setEnabled(rules.measurement);

  auto runs = experiment::build_runs(f, lab_.ids);
  QString text;
  bool ok = runs.has_value();
  if (!ok) {
    text = QString::fromStdString(runs.error().what);
  } else {
    // The new runs checked against the lab on their own (row numbers are theirs).
    experiment::QueueSpec probe = model_.queue();
    probe.runs = *runs;
    probe.queue_conditionals.clear();
    QStringList problems;
    for (const auto& d : experiment::lab::check_lab_queue(lab_, probe).all()) {
      if (d.run < 0 || d.severity != experiment::Severity::Error) continue;
      problems.append(QString::fromStdString(d.field + ": " + d.message));
    }
    problems.removeDuplicates();
    if (!problems.isEmpty()) {
      ok = false;
      text = problems.join(QLatin1Char('\n'));
    } else {
      const auto& first = runs->front();
      const auto& last = runs->back();
      auto label = [](const experiment::RunSpec& r) {
        QString t = QString::fromStdString(r.id.identifier);
        if (!r.id.step.empty()) t += QString::fromStdString(r.id.step);
        if (r.extraction.position) t += QStringLiteral(" @") + QString::fromStdString(experiment::format_position(*r.extraction.position));
        return t;
      };
      text = tr("Adds %n run(s): ", nullptr, static_cast<int>(runs->size())) + label(first) +
             (runs->size() > 1 ? QStringLiteral(" … ") + label(last) : QString());
    }
  }
  preview_->setText(text);
  style::set_tone(preview_, ok ? style::Tone::Normal : style::Tone::Error);
  add_->setEnabled(ok && !locked_ && !model_.locked());
}

std::size_t RunFactoryPanel::insert_at() const {
  const auto rows = selection_ ? selection_() : std::vector<std::size_t>{};
  if (!after_selection_->isChecked() || rows.empty()) return model_.queue().runs.size();
  // While a queue runs, never among the rows the executor has reached.
  return model_.insert_position(*std::max_element(rows.begin(), rows.end()) + 1);
}

void RunFactoryPanel::report_inserted(std::size_t at, std::size_t count) {
  std::vector<std::size_t> rows;
  for (std::size_t i = 0; i < count; ++i) rows.push_back(at + i);
  if (!rows.empty()) emit inserted(rows);
}

bool RunFactoryPanel::add() {
  if (!add_enabled()) return false;
  const FactoryForm f = form();
  auto runs = experiment::build_runs(f, lab_.ids);
  if (!runs) return false;
  const std::size_t at = insert_at();
  if (!model_.insert_runs(at, *runs)) return false;
  report_inserted(at, runs->size());
  const experiment::IncrementOptions inc{inc_identifier_->value(), inc_position_->value()};
  if (inc.identifier != 0 || inc.position != 0) {
    if (auto next = experiment::next_form(f, inc, lab_.ids)) set_form(*next);
  }
  refresh();
  return true;
}

bool RunFactoryPanel::apply_defaults() {
  auto d = experiment::with_lab_defaults(form(), lab_.ids, lab_.defaults);
  if (!d) {
    preview_->setText(tr("No defaults for %1 on device '%2' in defaults.toml")
                          .arg(q(experiment::to_string(lab_.ids.classify(s(identifier_->text())))), device_->text()));
    style::set_tone(preview_, style::Tone::Error);
    return false;
  }
  set_form(*d);
  return true;
}

bool RunFactoryPanel::load_from_selection() {
  const auto rows = selection_ ? selection_() : std::vector<std::size_t>{};
  if (rows.empty() || rows.front() >= model_.queue().runs.size()) return false;
  FactoryForm f = experiment::form_from_run(model_.queue().runs[rows.front()]);
  // Keep how the form expands, which a row does not carry.
  const FactoryForm current = form();
  f.one_run_per_hole = current.one_run_per_hole;
  f.identifier_step = current.identifier_step;
  set_form(f);
  return true;
}

bool RunFactoryPanel::insert_frequency() {
  if (locked_ || model_.locked()) return false;
  const auto type = static_cast<AnalysisType>(freq_type_->currentData().toInt());
  auto special = experiment::make_special_run(type, lab_.ids, s(device_->text()), lab_.defaults);
  if (!special) {
    preview_->setText(QString::fromStdString(special.error().what));
    return false;
  }
  experiment::FrequencySpec spec;
  spec.run = std::move(*special);
  spec.every = freq_every_->value();
  spec.before = freq_before_->isChecked();
  spec.after = freq_after_->isChecked();
  const auto rows = selection_ ? selection_() : std::vector<std::size_t>{};
  if (rows.size() > 1) {  // a range of rows limits where they go
    spec.first = rows.front();
    spec.last = rows.back() + 1;
  }
  // While a queue runs, only the rows the executor has not reached.
  spec.first = model_.insert_position(spec.first);
  if (spec.last && *spec.last < spec.first) return false;
  auto added = model_.add_frequency(spec);
  return added && *added > 0;
}

bool RunFactoryPanel::insert_block() {
  if (locked_ || model_.locked() || block_->count() == 0) return false;
  auto it = lab_.blocks.find(s(block_->currentText()));
  if (it == lab_.blocks.end()) return false;
  const auto runs = experiment::repeat_block(
      experiment::instantiate_block(it->second, {s(device_->text()), &lab_.defaults}), block_times_->value());
  const std::size_t at = insert_at();
  if (!model_.insert_runs(at, runs)) return false;
  report_inserted(at, runs.size());
  return true;
}

void RunFactoryPanel::set_increment(int identifier, int position) {
  inc_identifier_->setValue(identifier);
  inc_position_->setValue(position);
}

void RunFactoryPanel::set_insert_after_selection(bool on) { after_selection_->setChecked(on); }

void RunFactoryPanel::set_frequency(AnalysisType type, int every, bool before, bool after) {
  freq_type_->setCurrentIndex(std::max(0, freq_type_->findData(static_cast<int>(type))));
  freq_every_->setValue(every);
  freq_before_->setChecked(before);
  freq_after_->setChecked(after);
}

void RunFactoryPanel::set_block(const QString& name, int times) {
  block_->setCurrentIndex(std::max(0, block_->findText(name)));
  block_times_->setValue(times);
}

void RunFactoryPanel::set_locked(bool locked) {
  locked_ = locked;
  for (QWidget* g : groups_) g->setEnabled(!locked && !(g == block_box_ && lab_.blocks.empty()));
  refresh();
}

void RunFactoryPanel::set_identifier_source(IdentifierSource* source) {
  if (source_) disconnect(source_, nullptr, this, nullptr);
  source_ = source;
  // Whatever the old source still owes is no longer wanted.
  ++packages_serial_;
  packages_.clear();
  {
    const QSignalBlocker block(package_);
    package_->clear();
    package_->addItem(tr("(none)"));
  }
  package_->setToolTip(QString());
  clear_contents();
  level_->setEnabled(false);
  show_selects(false);
  if (!source_) return;
  connect(source_, &IdentifierSource::changed, this, [this] { load_packages(); });
  connect(source_, &QObject::destroyed, this, [this] { set_identifier_source(nullptr); });
  load_packages();
}

void RunFactoryPanel::show_selects(bool on) {
  run_form_->setRowVisible(package_, on);
  run_form_->setRowVisible(level_, on);
}

// The first load shows the selects; a later one (the catalog changed) keeps
// the chosen package and level when they are still there.
void RunFactoryPanel::load_packages() {
  if (!source_) return;
  const std::uint64_t serial = ++packages_serial_;
  source_->packages(this, [this, serial](Result<std::vector<PackageChoice>> answer) {
    if (serial != packages_serial_) return;
    if (!answer) {
      // Before the first list there is nothing to hang the reason on: the rows stay hidden.
      package_->setToolTip(QString::fromStdString(to_string(answer.error())));
      return;
    }
    package_->setToolTip(QString());
    const QString chosen = package_->currentIndex() > 0 ? package_->currentData().toString() : QString();
    packages_ = std::move(*answer);
    int index = 0;
    {
      const QSignalBlocker block(package_);
      package_->clear();
      package_->addItem(tr("(none)"));
      for (const auto& p : packages_) {
        package_->addItem(q(p.name), q(p.id));
        if (!chosen.isEmpty() && q(p.id) == chosen) index = package_->count() - 1;
      }
      package_->setCurrentIndex(index);
    }
    show_selects(true);
    if (chosen.isEmpty()) return;
    if (index == 0) {
      load_contents();  // the package is gone
      return;
    }
    request_contents(level_->currentIndex() > 0 ? std::optional(level_->currentText().toStdString()) : std::nullopt);
  });
}

void RunFactoryPanel::clear_contents() {
  ++contents_serial_;
  contents_ = {};
  fill_levels();
  fill_identifiers();
  identifier_select_->setToolTip(QString());
}

// The chosen package's levels and identifiers; nothing for "(none)".
void RunFactoryPanel::load_contents() {
  clear_contents();
  const bool chosen = package_->currentIndex() > 0;
  level_->setEnabled(chosen);
  if (chosen) request_contents(std::nullopt);
}

void RunFactoryPanel::request_contents(std::optional<std::string> keep_level) {
  if (!source_) return;
  const std::uint64_t serial = ++contents_serial_;
  source_->contents(this, package_->currentData().toString().toStdString(),
                    [this, serial, keep = std::move(keep_level)](Result<PackageContents> answer) {
                      if (serial != contents_serial_) return;
                      if (!answer) {
                        clear_contents();
                        identifier_select_->setToolTip(QString::fromStdString(to_string(answer.error())));
                        return;
                      }
                      identifier_select_->setToolTip(QString());
                      contents_ = std::move(*answer);
                      fill_levels();
                      if (keep) {
                        const auto it = std::find(contents_.levels.begin(), contents_.levels.end(), *keep);
                        if (it != contents_.levels.end()) {
                          const QSignalBlocker block(level_);
                          level_->setCurrentIndex(static_cast<int>(it - contents_.levels.begin()) + 1);
                        }
                      }
                      fill_identifiers();
                    });
}

void RunFactoryPanel::fill_levels() {
  const QSignalBlocker block(level_);
  level_->clear();
  level_->addItem(tr("(all)"));
  for (const auto& name : contents_.levels) level_->addItem(q(name));
}

void RunFactoryPanel::fill_identifiers() {
  const QString typed = identifier_->text();
  const bool all = level_->currentIndex() <= 0;
  const std::string level = level_->currentText().toStdString();
  const QSignalBlocker block(identifier_select_);
  identifier_select_->clear();
  for (const auto& c : contents_.choices) {
    if (!all && c.level != level) continue;
    const QString where = QStringLiteral("(%1 %2)").arg(q(c.level)).arg(c.position);
    QStringList parts{q(c.identifier)};
    if (!c.sample.empty()) parts.append(q(c.sample));
    parts.append(where);
    identifier_select_->addItem(parts.join(QStringLiteral("  ")), q(c.identifier));
  }
  identifier_select_->setCurrentIndex(-1);
  identifier_->setText(typed);
}

void RunFactoryPanel::choose_package(int index) {
  package_->setCurrentIndex(index);
  load_contents();
}

void RunFactoryPanel::choose_level(int index) {
  level_->setCurrentIndex(index);
  fill_identifiers();
}

void RunFactoryPanel::choose_identifier(int index) {
  if (index < 0 || index >= identifier_select_->count()) return;
  identifier_select_->setCurrentIndex(index);
  // The item says more than the identifier; the field holds the identifier.
  identifier_->setText(identifier_select_->itemData(index).toString());
  on_identifier_changed();
}

namespace {
QStringList item_texts(const QComboBox* c) {
  QStringList out;
  for (int i = 0; i < c->count(); ++i) out.append(c->itemText(i));
  return out;
}
}  // namespace

QStringList RunFactoryPanel::package_choices() const { return item_texts(package_); }
QStringList RunFactoryPanel::level_choices() const { return item_texts(level_); }
QStringList RunFactoryPanel::identifier_choices() const { return item_texts(identifier_select_); }
bool RunFactoryPanel::selects_visible() const { return run_form_->isRowVisible(package_); }
QString RunFactoryPanel::identifier_tooltip() const { return identifier_select_->toolTip(); }
QString RunFactoryPanel::package_tooltip() const { return package_->toolTip(); }

QString RunFactoryPanel::preview_text() const { return preview_->text(); }
bool RunFactoryPanel::add_enabled() const { return add_->isEnabled(); }

bool RunFactoryPanel::field_enabled(const char* name) const {
  const auto* w = findChild<QWidget*>(QString::fromLatin1(name));
  return w != nullptr && w->isEnabled();
}

void RunFactoryPanel::refresh_conditionals() {
  QStringList names;
  if (auto files = lab_.condition_files->list())
    for (const auto& n : *files) names.append(q(n));
  // A ticked file the lab no longer has stays listed, so it can be unticked.
  for (const auto& n : conditionals_)
    if (!names.contains(q(n))) names.append(q(n));
  QMenu* menu = conditionals_button_->menu();
  menu->clear();
  for (const QString& name : names) {
    QAction* a = menu->addAction(name);
    a->setCheckable(true);
    a->setChecked(std::find(conditionals_.begin(), conditionals_.end(), name.toStdString()) != conditionals_.end());
    connect(a, &QAction::toggled, this, [this, name](bool on) { set_conditional_checked(name, on); });
  }
  conditionals_button_->setText(conditionals_text());
}

void RunFactoryPanel::set_conditional_checked(const QString& name, bool on) {
  const std::string n = name.toStdString();
  const auto it = std::find(conditionals_.begin(), conditionals_.end(), n);
  if (on == (it != conditionals_.end())) return;
  if (on) conditionals_.push_back(n);
  else conditionals_.erase(it);
  for (QAction* a : conditionals_button_->menu()->actions())
    if (a->text() == name && a->isChecked() != on) a->setChecked(on);
  conditionals_button_->setText(conditionals_text());
  if (!updating_) refresh();
}

QStringList RunFactoryPanel::conditional_choices() const {
  QStringList out;
  for (const QAction* a : conditionals_button_->menu()->actions()) out.append(a->text());
  return out;
}

QString RunFactoryPanel::conditionals_text() const {
  QStringList names;
  for (const auto& n : conditionals_) names.append(q(n));
  return names.isEmpty() ? tr("(none)") : names.join(QStringLiteral(", "));
}

QStringList RunFactoryPanel::plan_choices() const {
  QStringList out;
  for (int i = 0; i < plan_->count(); ++i)
    if (!plan_->itemText(i).isEmpty()) out.append(plan_->itemText(i));
  return out;
}

QStringList RunFactoryPanel::script_choices() const {
  QStringList out;
  for (int i = 0; i < script_->count(); ++i)
    if (!script_->itemText(i).isEmpty()) out.append(script_->itemText(i));
  return out;
}

QStringList RunFactoryPanel::block_choices() const {
  QStringList out;
  for (int i = 0; i < block_->count(); ++i) out.append(block_->itemText(i));
  return out;
}

}  // namespace pychron::ui
