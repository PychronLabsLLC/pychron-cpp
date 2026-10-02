#include "run_factory_panel.hpp"

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
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
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
  identifier_ = new QLineEdit;
  identifier_->setObjectName(QStringLiteral("identifier"));
  identifier_->setPlaceholderText(tr("e.g. 66001, or a special such as bu"));
  aliquot_ = new QLineEdit;
  aliquot_->setPlaceholderText(tr("auto"));
  aliquot_->setValidator(new QIntValidator(1, 1'000'000, aliquot_));
  step_ = new QLineEdit;
  form->addRow(tr("Type"), type_);
  form->addRow(tr("Identifier"), identifier_);
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
  form->addRow(tr("Comment"), comment_);
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
  add_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
  add_->setToolTip(tr("Add the runs (Ctrl+Return)"));
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
  preview_->setStyleSheet(ok ? QString() : QStringLiteral("color: #a01818;"));
  add_->setEnabled(ok && !locked_ && !model_.locked());
}

std::size_t RunFactoryPanel::insert_at() const {
  const auto rows = selection_ ? selection_() : std::vector<std::size_t>{};
  if (!after_selection_->isChecked() || rows.empty()) return model_.queue().runs.size();
  return *std::max_element(rows.begin(), rows.end()) + 1;
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
    preview_->setStyleSheet(QStringLiteral("color: #a01818;"));
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

QString RunFactoryPanel::preview_text() const { return preview_->text(); }
bool RunFactoryPanel::add_enabled() const { return add_->isEnabled(); }

bool RunFactoryPanel::field_enabled(const char* name) const {
  const auto* w = findChild<QWidget*>(QString::fromLatin1(name));
  return w != nullptr && w->isEnabled();
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
