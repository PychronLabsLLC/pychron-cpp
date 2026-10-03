#include "measurement_panel.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

namespace plan = experiment::plan;
using experiment::MeasurementRef;
using experiment::ParamValue;

QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString clock_text(experiment::Duration d) {
  const auto total = static_cast<long long>(std::llround(d.count()));
  return QStringLiteral("%1:%2:%3")
      .arg(total / 3600)
      .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

plan::ParamKind kind_of(const ParamValue& v) {
  if (std::holds_alternative<bool>(v)) return plan::ParamKind::Bool;
  if (std::holds_alternative<std::int64_t>(v)) return plan::ParamKind::Int;
  if (std::holds_alternative<double>(v)) return plan::ParamKind::Float;
  return plan::ParamKind::String;
}

}  // namespace

MeasurementPanel::MeasurementPanel(const experiment::lab::Lab& lab, QueueTableModel& model, Selection selection,
                                   QWidget* parent)
    : QWidget(parent),
      lab_(lab),
      model_(model),
      selection_(std::move(selection)),
      header_(new QLabel),
      family_(new QComboBox),
      plan_(new QComboBox),
      description_(new QLabel),
      advanced_(new QCheckBox(tr("Advanced: override any value of the plan"))),
      scroll_(new QScrollArea),
      reset_all_(new QPushButton(tr("Reset All"))),
      status_(new QLabel) {
  header_->setWordWrap(true);
  description_->setWordWrap(true);
  style::set_tone(description_, style::Tone::Muted);
  status_->setWordWrap(true);
  family_->setObjectName(QStringLiteral("family"));
  plan_->setObjectName(QStringLiteral("plan"));
  family_->addItem(tr("All families"), QString());
  for (const auto& f : plan::plan_families(*lab_.plans)) family_->addItem(q(f), q(f));

  auto* tmpl = new QGroupBox(tr("Template"));
  auto* tform = new QFormLayout(tmpl);
  tform->addRow(tr("Family"), family_);
  tform->addRow(tr("Plan"), plan_);
  tform->addRow(description_);

  auto* params = new QGroupBox(tr("Parameters"));
  auto* pcol = new QVBoxLayout(params);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setFrameShape(QFrame::NoFrame);
  pcol->addWidget(advanced_);
  pcol->addWidget(scroll_, 1);
  auto* reset_row = new QHBoxLayout;
  reset_row->addStretch(1);
  reset_row->addWidget(reset_all_);
  pcol->addLayout(reset_row);

  auto* column = new QVBoxLayout(this);
  column->addWidget(header_);
  column->addWidget(tmpl);
  column->addWidget(params, 1);
  column->addWidget(status_);

  connect(family_, &QComboBox::activated, this, [this] { fill_plans(); });
  connect(plan_, &QComboBox::activated, this, [this] {
    if (!choose_plan(plan_->currentData().toString())) rebuild();
  });
  connect(advanced_, &QCheckBox::clicked, this, [this](bool on) {
    if (!set_advanced(on)) rebuild();
  });
  connect(reset_all_, &QPushButton::clicked, this, [this] { reset_all(); });
  connect(&model_, &QueueTableModel::validated, this, [this] { sync(); });
  connect(&model_, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
  connect(&model_, &QueueTableModel::frozenChanged, this, [this] { update_marks(); });
  refresh();
}

const experiment::RunSpec* MeasurementPanel::run() const {
  if (!row_ || *row_ >= model_.queue().runs.size()) return nullptr;
  return &model_.queue().runs[*row_];
}

void MeasurementPanel::refresh() {
  const auto rows = selection_ ? selection_() : std::vector<std::size_t>{};
  row_.reset();
  if (rows.size() == 1 && rows.front() < model_.queue().runs.size()) row_ = rows.front();
  rebuild();
}

void MeasurementPanel::rebuild() {
  building_ = true;
  const experiment::RunSpec* r = run();
  if (r == nullptr) {
    header_->setText(tr("Select one run to edit its measurement."));
    shown_ = {};
  } else {
    header_->setText(tr("Row %1 · %2 (%3)")
                         .arg(*row_)
                         .arg(q(r->id.identifier), q(experiment::to_string(r->id.type))));
    shown_ = r->measurement;
  }
  advanced_->setChecked(shown_.advanced);
  fill_plans();
  build_parameters();
  building_ = false;
  update_marks();
}

void MeasurementPanel::fill_plans() {
  const QString family = family_->currentData().toString();
  plan_->clear();
  const experiment::RunSpec* r = run();
  if (r == nullptr) {
    description_->clear();
    return;
  }
  plan_->addItem(tr("(no plan)"), QString());
  const auto names = plan::matching_plans(*lab_.plans, family.toStdString(), r->id.type);
  // The row's own plan stays choosable even when the filter would hide it.
  if (!shown_.plan.empty() && std::find(names.begin(), names.end(), shown_.plan) == names.end()) {
    plan_->addItem(q(shown_.plan) + (lab_.plans->find(shown_.plan) ? tr(" (other family or type)") : tr(" (unknown)")),
                   q(shown_.plan));
  }
  for (const auto& n : names) plan_->addItem(q(n), q(n));
  plan_->setCurrentIndex(std::max(0, plan_->findData(q(shown_.plan))));
  if (const auto* t = lab_.plans->find(shown_.plan)) {
    const auto info = plan::plan_info(*t);
    QString text = q(info.description);
    if (!info.instrument_family.empty()) text += (text.isEmpty() ? QString() : QStringLiteral(" · ")) + q(info.instrument_family);
    description_->setText(text);
  } else {
    description_->clear();
  }
}

void MeasurementPanel::build_parameters() {
  // A fresh widget per build. The old one may be mid-signal (a Reset button
  // that rebuilt the panel), so it is taken out of the scroll area and
  // deleted later.
  if (QWidget* old = scroll_->takeWidget()) {
    old->hide();
    old->deleteLater();
  }
  editors_.clear();
  order_.clear();
  auto* box = new QWidget;
  auto* form = new QFormLayout(box);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);  // long paths go above their editor
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  std::vector<plan::PlanParameter> params;
  if (const auto* t = lab_.plans->find(shown_.plan)) {
    if (auto ps = plan::plan_parameters(*t, shown_.advanced)) params = std::move(*ps);
  }
  const std::size_t from_template = params.size();
  // Overrides with no parameter (not exposed, or gone from the template) are
  // listed too, so they can be seen and reset.
  for (const auto& [path, value] : shown_.overrides) {
    if (std::any_of(params.begin(), params.end(), [&](const auto& p) { return p.path == path; })) continue;
    plan::PlanParameter extra;
    extra.path = path;
    extra.label = path + " (not a parameter)";
    extra.kind = kind_of(value);
    extra.value = value;
    params.push_back(std::move(extra));
  }

  std::string section;  // Advanced: values are grouped by their top-level table
  for (std::size_t i = 0; i < params.size(); ++i) {
    const plan::PlanParameter& p = params[i];
    Editor e;
    e.param = p;
    e.extra = i >= from_template;
    QString label = q(p.label);
    if (shown_.advanced && !e.extra && p.label == p.path) {
      const auto dot = p.path.find_first_of(".[");
      const std::string top = p.path.substr(0, dot);
      if (top != section) {
        section = top;
        auto* header = new QLabel(QStringLiteral("<b>%1</b>").arg(q(top)));
        form->addRow(header);
      }
      if (dot != std::string::npos) label = q(p.path.substr(dot + (p.path[dot] == '.' ? 1 : 0)));
    }
    e.label = new QLabel(label);
    e.label->setToolTip(q(p.path) + QStringLiteral(" (") + q(plan::to_string(p.kind)) + QStringLiteral(")"));
    auto* field = new QWidget;
    auto* row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    if (p.kind == plan::ParamKind::Bool) {
      e.check = new QCheckBox;
      e.check->setObjectName(q(p.path));
      row->addWidget(e.check);
      row->addStretch(1);
    } else {
      e.line = new QLineEdit;
      e.line->setObjectName(q(p.path));
      e.line->setMinimumWidth(80);
      if (p.kind == plan::ParamKind::Alias) e.line->setPlaceholderText(q(plan::format_param(p.value)));
      row->addWidget(e.line, 1);
    }
    e.badge = new QLabel;
    e.badge->setFixedWidth(12);
    e.reset = new QPushButton(tr("Reset"));
    e.reset->setFlat(true);
    row->addWidget(e.badge);
    row->addWidget(e.reset);
    form->addRow(e.label, field);

    const std::string path = p.path;
    order_.push_back(path);
    auto [it, inserted] = editors_.emplace(path, std::move(e));
    Editor& ed = it->second;
    const auto it_override = shown_.overrides.find(path);
    const ParamValue effective = it_override != shown_.overrides.end() ? it_override->second : ed.param.value;
    if (ed.check) ed.check->setChecked(std::holds_alternative<bool>(effective) && std::get<bool>(effective));
    if (ed.line) ed.line->setText(q(plan::format_param(effective)));
    if (ed.check)
      connect(ed.check, &QCheckBox::toggled, this, [this, path] {
        if (auto it2 = editors_.find(path); it2 != editors_.end()) commit(it2->second);
      });
    if (ed.line)
      connect(ed.line, &QLineEdit::editingFinished, this, [this, path] {
        if (auto it2 = editors_.find(path); it2 != editors_.end()) commit(it2->second);
      });
    connect(ed.reset, &QPushButton::clicked, this, [this, path] { reset_parameter(q(path)); });
  }
  if (editors_.empty()) {
    auto* none = new QLabel(shown_.plan.empty() ? tr("No plan.")
                                                : tr("The plan exposes no parameters; use Advanced to change it."));
    none->setWordWrap(true);
    form->addRow(none);
  }
  scroll_->setWidget(box);
}

void MeasurementPanel::update_marks() {
  const bool editable = run() != nullptr && !locked_ && model_.row_editable(*row_);
  family_->setEnabled(run() != nullptr);
  plan_->setEnabled(editable);
  advanced_->setEnabled(editable && !shown_.plan.empty());
  reset_all_->setEnabled(editable && !shown_.overrides.empty());
  for (auto& [path, e] : editors_) {
    const auto it = shown_.overrides.find(path);
    const bool over = it != shown_.overrides.end();
    QFont f = e.label->font();
    f.setBold(over);
    e.label->setFont(f);
    e.badge->setText(over ? QStringLiteral("●") : QString());
    style::set_tone(e.badge, style::Tone::Accent);
    e.badge->setToolTip(over ? tr("Overridden; the plan has %1").arg(q(plan::format_param(e.param.value))) : QString());
    e.reset->setVisible(over);
    e.reset->setEnabled(editable);
    if (e.line) e.line->setReadOnly(!editable);
    if (e.check) e.check->setEnabled(editable);
  }

  const experiment::RunSpec* r = run();
  if (r == nullptr) {
    status_->clear();
    status_ok_ = false;
    return;
  }
  if (shown_.plan.empty()) {
    status_ok_ = !experiment::rules_for(r->id.type).measurement;
    status_->setText(status_ok_ ? tr("This run type takes no measurement.") : tr("No measurement plan."));
  } else {
    auto loaded = lab_.plans->load(shown_.plan, shown_.overrides, plan::LoadOptions{shown_.advanced});
    status_ok_ = loaded.has_value();
    if (!loaded) {
      status_->setText(q(loaded.error().what));
    } else {
      const auto d = lab_.plans->plan_duration(shown_.plan, shown_.overrides, shown_.advanced);
      status_->setText(tr("Measures %1 · %n override(s)", nullptr, static_cast<int>(shown_.overrides.size()))
                           .arg(d ? clock_text(*d) : tr("?")));
    }
  }
  style::set_tone(status_, status_ok_ ? style::Tone::Normal : style::Tone::Error);
}

bool MeasurementPanel::store(MeasurementRef measurement) {
  const experiment::RunSpec* r = run();
  if (r == nullptr || locked_ || !model_.row_editable(*row_)) return false;
  experiment::RunSpec next = *r;
  next.measurement = std::move(measurement);
  const MeasurementRef before = shown_;
  shown_ = next.measurement;  // set first: the model's validated signal must not rebuild
  if (!model_.replace_run(*row_, std::move(next))) {
    shown_ = before;
    return false;
  }
  update_marks();
  return true;
}

void MeasurementPanel::commit(Editor& e) {
  if (building_) return;
  std::optional<ParamValue> value;
  if (e.check) value = e.check->isChecked();
  else value = plan::parse_param(e.param.kind, e.line->text().toStdString());
  const auto it = shown_.overrides.find(e.param.path);
  const ParamValue current = it != shown_.overrides.end() ? it->second : e.param.value;
  if (!value) {
    if (e.line) e.line->setText(q(plan::format_param(current)));  // revert text that does not fit
    return;
  }
  if (plan::same_param(*value, current)) return;  // unchanged
  MeasurementRef m = shown_;
  if (!e.extra && plan::same_param(*value, e.param.value)) m.overrides.erase(e.param.path);  // back to the plan's
  else m.overrides[e.param.path] = *value;
  if (!store(std::move(m))) {
    if (e.line) e.line->setText(q(plan::format_param(current)));
    if (e.check) {
      const QSignalBlocker block(e.check);
      e.check->setChecked(std::holds_alternative<bool>(current) && std::get<bool>(current));
    }
  }
}

void MeasurementPanel::sync() {
  const experiment::RunSpec* r = run();
  if (r == nullptr || r->measurement != shown_) {
    rebuild();
    return;
  }
  update_marks();
}

QStringList MeasurementPanel::families() const {
  QStringList out;
  for (int i = 0; i < family_->count(); ++i) out.append(family_->itemData(i).toString());
  return out;
}

QStringList MeasurementPanel::plans() const {
  QStringList out;
  for (int i = 0; i < plan_->count(); ++i)
    if (!plan_->itemData(i).toString().isEmpty()) out.append(plan_->itemData(i).toString());
  return out;
}

bool MeasurementPanel::set_family(const QString& family) {
  const int i = family_->findData(family);
  if (i < 0) return false;
  family_->setCurrentIndex(i);
  fill_plans();
  return true;
}

bool MeasurementPanel::choose_plan(const QString& name) {
  if (run() == nullptr) return false;
  const std::string n = name.toStdString();
  if (n == shown_.plan) return true;
  MeasurementRef m = shown_;
  m.plan = n;
  if (n.empty()) {  // no plan: nothing to override
    m.overrides.clear();
    m.advanced = false;
  } else if (const auto* t = lab_.plans->find(n)) {
    m.overrides = plan::overrides_for(*t, m.overrides, m.advanced);
  } else {
    return false;
  }
  if (!store(std::move(m))) return false;
  rebuild();
  return true;
}

bool MeasurementPanel::set_advanced(bool on) {
  if (run() == nullptr || shown_.plan.empty()) return false;
  if (on == shown_.advanced) return true;
  MeasurementRef m = shown_;
  m.advanced = on;  // overrides stay; any that need Advanced show as errors until reset
  if (!store(std::move(m))) return false;
  rebuild();
  return true;
}

bool MeasurementPanel::advanced() const { return shown_.advanced; }

QStringList MeasurementPanel::parameter_paths() const {
  QStringList out;
  for (const auto& p : order_) out.append(q(p));
  return out;
}

QString MeasurementPanel::parameter_text(const QString& path) const {
  auto it = editors_.find(path.toStdString());
  if (it == editors_.end()) return {};
  if (it->second.check) return it->second.check->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
  return it->second.line->text();
}

bool MeasurementPanel::overridden(const QString& path) const { return shown_.overrides.contains(path.toStdString()); }

bool MeasurementPanel::set_parameter(const QString& path, const QString& text) {
  auto it = editors_.find(path.toStdString());
  if (it == editors_.end() || run() == nullptr || locked_ || !model_.row_editable(*row_)) return false;
  Editor& e = it->second;
  const auto value = plan::parse_param(e.check ? plan::ParamKind::Bool : e.param.kind, text.toStdString());
  if (!value) return false;
  const MeasurementRef before = shown_;
  const auto o = shown_.overrides.find(e.param.path);
  const ParamValue current = o != shown_.overrides.end() ? o->second : e.param.value;
  if (e.check) {
    const QSignalBlocker block(e.check);
    e.check->setChecked(std::get<bool>(*value));
  } else {
    e.line->setText(text);
  }
  commit(e);
  return shown_ != before || plan::same_param(*value, current);
}

bool MeasurementPanel::reset_parameter(const QString& path) {
  const std::string p = path.toStdString();
  if (!shown_.overrides.contains(p)) return false;
  MeasurementRef m = shown_;
  m.overrides.erase(p);
  if (!store(std::move(m))) return false;
  rebuild();
  return true;
}

bool MeasurementPanel::reset_all() {
  if (shown_.overrides.empty()) return false;
  MeasurementRef m = shown_;
  m.overrides.clear();
  if (!store(std::move(m))) return false;
  rebuild();
  return true;
}

QString MeasurementPanel::header_text() const { return header_->text(); }
QString MeasurementPanel::status_text() const { return status_->text(); }

void MeasurementPanel::set_locked(bool locked) {
  locked_ = locked;
  update_marks();
}

}  // namespace pychron::ui
