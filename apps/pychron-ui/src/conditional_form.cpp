#include "conditional_form.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QSpinBox>

#include "theme.hpp"

namespace pychron::ui {

using experiment::ActionSpec;
using experiment::Conditional;
using experiment::ConditionalKind;

namespace {

// The action's word, as written in a file ("skip_n", "set_param", ...).
QString action_word(ActionSpec::Type type) {
  ActionSpec a;
  a.type = type;
  const QString text = QString::fromStdString(to_string(a));
  return text.section(QLatin1Char(' '), 0, 0);
}

QWidget* row_of(std::initializer_list<QWidget*> widgets) {
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  for (QWidget* w : widgets) layout->addWidget(w);
  layout->addStretch(1);
  return row;
}

QLabel* error_label() {
  auto* label = new QLabel;
  label->setWordWrap(true);
  QPalette p = label->palette();
  p.setColor(QPalette::WindowText, theme().error_text);
  label->setPalette(p);
  label->hide();
  return label;
}

}  // namespace

ConditionalForm::ConditionalForm(QStringList analysis_types, QWidget* parent)
    : QWidget(parent),
      offered_types_(std::move(analysis_types)),
      name_(new QLineEdit),
      kind_(new QComboBox),
      check_(new QLineEdit),
      check_error_(error_label()),
      start_(new QSpinBox),
      frequency_(new QSpinBox),
      ntrips_(new QSpinBox),
      window_(new QSpinBox),
      mapper_(new QLineEdit),
      types_(new QListWidget),
      ratio_(new QDoubleSpinBox),
      action_(new QComboBox),
      action_quick_(new QCheckBox(tr("quick"))),
      action_name_(new QLineEdit),
      action_value_(new QLineEdit),
      action_count_(new QSpinBox),
      action_steps_(new QLineEdit),
      action_percent_(new QCheckBox(tr("percent"))),
      action_error_(error_label()),
      resume_(new QCheckBox(tr("Resume (keep measuring and re-arm)"))),
      run_flag_(new QComboBox) {
  name_->setObjectName(QStringLiteral("name"));
  kind_->setObjectName(QStringLiteral("kind"));
  check_->setObjectName(QStringLiteral("check"));
  start_->setObjectName(QStringLiteral("start"));
  frequency_->setObjectName(QStringLiteral("frequency"));
  ntrips_->setObjectName(QStringLiteral("ntrips"));
  window_->setObjectName(QStringLiteral("window"));
  mapper_->setObjectName(QStringLiteral("mapper"));
  types_->setObjectName(QStringLiteral("types"));
  ratio_->setObjectName(QStringLiteral("ratio"));
  action_->setObjectName(QStringLiteral("action"));
  action_quick_->setObjectName(QStringLiteral("action_quick"));
  action_name_->setObjectName(QStringLiteral("action_name"));
  action_value_->setObjectName(QStringLiteral("action_value"));
  action_count_->setObjectName(QStringLiteral("action_count"));
  action_steps_->setObjectName(QStringLiteral("action_steps"));
  action_percent_->setObjectName(QStringLiteral("action_percent"));
  resume_->setObjectName(QStringLiteral("resume"));
  run_flag_->setObjectName(QStringLiteral("run_flag"));

  for (const ConditionalKind k : experiment::kFileOrder)
    kind_->addItem(QString::fromUtf8(to_string(k)), static_cast<int>(k));
  check_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  check_->setPlaceholderText(tr("e.g. Ar40 > 8e5"));
  start_->setRange(0, 1000000);
  frequency_->setRange(1, 1000000);
  ntrips_->setRange(1, 1000000);
  window_->setRange(0, 1000000);
  window_->setSpecialValueText(tr("none"));
  mapper_->setPlaceholderText(tr("e.g. x + 1000"));
  types_->setFlow(QListView::LeftToRight);
  types_->setWrapping(true);
  types_->setResizeMode(QListView::Adjust);
  types_->setMaximumHeight(60);
  types_->setToolTip(tr("None ticked: every analysis type. \"blank\" covers every blank type."));
  ratio_->setDecimals(4);
  ratio_->setRange(0.0001, 1.0);
  ratio_->setSingleStep(0.05);
  ratio_->setToolTip(tr("Count scale after a truncation this conditional causes"));
  action_name_->setPlaceholderText(tr("name"));
  action_value_->setPlaceholderText(tr("value"));
  action_count_->setRange(1, 1000000);
  action_steps_->setPlaceholderText(tr("steps, e.g. 1,2,3"));
  run_flag_->addItems({tr("none"), QStringLiteral("truncate"), QStringLiteral("terminate")});
  run_flag_->setToolTip(tr("Also truncate or terminate the run"));

  gating_ = row_of({new QLabel(tr("Start")), start_, new QLabel(tr("Frequency")), frequency_});
  ratio_row_ = row_of({ratio_});
  action_row_ = row_of(
      {action_, action_quick_, action_name_, action_value_, action_count_, action_steps_, action_percent_});
  run_flag_row_ = row_of({run_flag_});

  auto* form = new QFormLayout(this);
  form->addRow(tr("Name"), row_of({name_, new QLabel(tr("Kind")), kind_}));
  form->addRow(tr("Check"), check_);
  form->addRow(QString(), check_error_);
  form->addRow(tr("Gating"), gating_);
  form->addRow(tr("Trips"), row_of({ntrips_, new QLabel(tr("Window")), window_, new QLabel(tr("Mapper")), mapper_}));
  form->addRow(tr("Types"), types_);
  form->addRow(tr("Count ratio"), ratio_row_);
  form->addRow(tr("Action"), action_row_);
  form->addRow(QString(), action_error_);
  form->addRow(QString(), resume_);
  form->addRow(tr("Run"), run_flag_row_);

  connect(name_, &QLineEdit::textEdited, this, [this](const QString& t) {
    c_.name = t.trimmed().toStdString();
    notify();
  });
  connect(kind_, &QComboBox::activated, this,
          [this](int i) { change_kind(static_cast<ConditionalKind>(kind_->itemData(i).toInt())); });
  connect(check_, &QLineEdit::textEdited, this, [this](const QString& t) {
    c_.check = t.toStdString();
    name_->setPlaceholderText(QString::fromStdString(experiment::default_name(c_.kind, c_.check)));
    update_check_error();
    notify();
  });
  auto spin = [this](QSpinBox* box, auto apply) {
    connect(box, &QSpinBox::valueChanged, this, [this, apply](int v) {
      if (loading_) return;
      apply(v);
      update_check_error();  // the window is part of the compiled check
      notify();
    });
  };
  spin(start_, [this](int v) { c_.start = v; });
  spin(frequency_, [this](int v) { c_.frequency = v; });
  spin(ntrips_, [this](int v) { c_.ntrips = v; });
  spin(window_, [this](int v) { c_.window = v > 0 ? std::optional<int>(v) : std::nullopt; });
  spin(action_count_, [this](int v) { c_.action.count = v; });
  connect(mapper_, &QLineEdit::textEdited, this, [this](const QString& t) {
    c_.mapper = t.trimmed().toStdString();
    update_check_error();
    notify();
  });
  connect(types_, &QListWidget::itemChanged, this, [this] {
    if (loading_) return;
    c_.analysis_types.clear();
    for (int i = 0; i < types_->count(); ++i)
      if (types_->item(i)->checkState() == Qt::Checked) c_.analysis_types.push_back(types_->item(i)->text().toStdString());
    notify();
  });
  connect(ratio_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
    if (loading_) return;
    c_.abbreviated_count_ratio = v;
    notify();
  });
  connect(action_, &QComboBox::activated, this,
          [this](int i) { change_action_type(static_cast<ActionSpec::Type>(action_->itemData(i).toInt())); });
  connect(action_quick_, &QCheckBox::toggled, this, [this](bool on) {
    if (loading_) return;
    c_.action.quick = on;
    notify();
  });
  connect(action_name_, &QLineEdit::textEdited, this, [this](const QString& t) {
    c_.action.name = t.trimmed().toStdString();
    notify();
  });
  connect(action_value_, &QLineEdit::textEdited, this, [this](const QString& t) {
    bool ok = false;
    const double v = t.trimmed().toDouble(&ok);
    action_error_->setText(ok ? QString() : tr("'%1' is not a number").arg(t));
    action_error_->setVisible(!ok);
    if (!ok) return;
    c_.action.value = v;
    notify();
  });
  connect(action_steps_, &QLineEdit::textEdited, this, [this] { read_steps(); });
  connect(action_percent_, &QCheckBox::toggled, this, [this] {
    if (!loading_) read_steps();
  });
  connect(resume_, &QCheckBox::toggled, this, [this](bool on) {
    if (loading_) return;
    c_.resume = on;
    notify();
  });
  connect(run_flag_, &QComboBox::activated, this, [this](int i) {
    c_.truncate = i == 1;
    c_.terminate = i == 2;
    notify();
  });

  set_conditional({});
}

void ConditionalForm::notify() {
  if (!loading_) emit edited(c_);
}

void ConditionalForm::set_conditional(const Conditional& c) {
  loading_ = true;
  c_ = c;
  c_.expr.reset();
  name_->setText(QString::fromStdString(c_.name));
  name_->setPlaceholderText(QString::fromStdString(experiment::default_name(c_.kind, c_.check)));
  kind_->setCurrentIndex(kind_->findData(static_cast<int>(c_.kind)));
  check_->setText(QString::fromStdString(c_.check));
  start_->setValue(c_.start);
  frequency_->setValue(c_.frequency);
  ntrips_->setValue(c_.ntrips);
  window_->setValue(c_.window.value_or(0));
  mapper_->setText(QString::fromStdString(c_.mapper));
  ratio_->setValue(c_.abbreviated_count_ratio);
  resume_->setChecked(c_.resume);
  run_flag_->setCurrentIndex(c_.truncate ? 1 : c_.terminate ? 2 : 0);
  fill_types();
  show_fields();
  action_error_->hide();
  update_check_error();
  loading_ = false;
}

void ConditionalForm::fill_types() {
  types_->clear();
  QStringList names = offered_types_;
  for (const auto& t : c_.analysis_types)
    if (const QString q = QString::fromStdString(t); !names.contains(q)) names.append(q);
  for (const QString& n : names) {
    auto* item = new QListWidgetItem(n, types_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    const bool on = std::find(c_.analysis_types.begin(), c_.analysis_types.end(), n.toStdString()) != c_.analysis_types.end();
    item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
  }
}

void ConditionalForm::show_fields() {
  const auto& f = experiment::fields_of(c_.kind);
  gating_->setVisible(f.gating);
  ratio_row_->setVisible(f.ratio);
  action_row_->setVisible(!f.actions.empty());
  resume_->setVisible(f.resume);
  run_flag_row_->setVisible(f.run_flags);
  const QSignalBlocker block(action_);
  action_->clear();
  for (const ActionSpec::Type t : f.actions) action_->addItem(action_word(t), static_cast<int>(t));
  action_->setCurrentIndex(action_->findData(static_cast<int>(c_.action.type)));
  show_action_params();
}

void ConditionalForm::show_action_params() {
  using T = ActionSpec::Type;
  const bool was = loading_;
  loading_ = true;
  const T t = c_.action.type;
  action_quick_->setVisible(t == T::Truncate);
  action_name_->setVisible(t == T::SetParam || t == T::RunHook);
  action_value_->setVisible(t == T::SetParam);
  action_count_->setVisible(t == T::SkipN);
  action_steps_->setVisible(t == T::SetExtract);
  action_percent_->setVisible(t == T::SetExtract);
  action_quick_->setChecked(c_.action.quick);
  action_name_->setText(QString::fromStdString(c_.action.name));
  action_value_->setText(QString::number(c_.action.value, 'g', 15));
  action_count_->setValue(c_.action.count);
  QStringList steps;
  for (const double s : c_.action.steps) steps.append(QString::number(s, 'g', 15));
  action_steps_->setText(steps.join(QLatin1Char(',')));
  action_percent_->setChecked(c_.action.percent);
  loading_ = was;
}

void ConditionalForm::update_check_error() {
  QString err;
  if (!QString::fromStdString(c_.check).trimmed().isEmpty()) {
    auto e = experiment::compile_check(c_.check, c_.window, c_.mapper);
    if (!e) err = QString::fromStdString(e.error().what);
  }
  check_error_->setText(err);
  check_error_->setVisible(!err.isEmpty());
}

QString ConditionalForm::check_error() const { return check_error_->text(); }

QString ConditionalForm::action_error() const { return action_error_->isHidden() ? QString() : action_error_->text(); }

void ConditionalForm::change_kind(ConditionalKind kind) {
  if (kind == c_.kind) return;
  const auto& f = experiment::fields_of(kind);
  c_.kind = kind;
  c_.action = {};
  c_.action.type = f.default_action;
  if (!f.gating) {
    c_.start = 0;
    c_.frequency = 1;
  }
  if (!f.ratio) c_.abbreviated_count_ratio = 1.0;
  if (!f.resume) c_.resume = false;
  if (!f.run_flags) c_.truncate = c_.terminate = false;
  const Conditional next = c_;
  set_conditional(next);
  emit edited(c_);
}

void ConditionalForm::change_action_type(ActionSpec::Type type) {
  if (type == c_.action.type) return;
  c_.action = {};
  c_.action.type = type;
  action_error_->hide();
  show_action_params();
  notify();
}

void ConditionalForm::read_steps() {
  QString text = action_steps_->text();
  const bool typed_percent = text.contains(QLatin1Char('%'));
  text.remove(QLatin1Char('%'));
  const bool percent = typed_percent || action_percent_->isChecked();
  QStringList parts = text.split(QLatin1Char(','));
  for (QString& p : parts) p = p.trimmed() + (percent ? QStringLiteral("%") : QString());
  auto a = experiment::parse_action("set_extract " + parts.join(QLatin1Char(',')).toStdString());
  action_error_->setText(a ? QString() : QString::fromStdString(a.error().what));
  action_error_->setVisible(!a);
  if (!a) return;
  c_.action = *a;
  if (typed_percent) {
    const QSignalBlocker block(action_percent_);
    action_percent_->setChecked(true);
  }
  notify();
}

bool ConditionalForm::shows_gating() const { return !gating_->isHidden(); }
bool ConditionalForm::shows_ratio() const { return !ratio_row_->isHidden(); }
bool ConditionalForm::shows_action() const { return !action_row_->isHidden(); }
bool ConditionalForm::shows_resume() const { return !resume_->isHidden(); }
bool ConditionalForm::shows_run_flags() const { return !run_flag_row_->isHidden(); }

}  // namespace pychron::ui
