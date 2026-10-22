#include "peak_center_panel.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "spectrometer_bridge.hpp"

namespace pychron::ui {

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }
Duration duration_of(double s) { return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(s)); }

// Native units, as the result area and the confirmation show them.
QString native(double v) { return QString::number(v, 'f', 6); }

QDoubleSpinBox* spin(double min, double max, int decimals, double step, const QString& suffix = {}) {
  auto* box = new QDoubleSpinBox;
  box->setDecimals(decimals);
  box->setRange(min, max);
  box->setSingleStep(step);
  box->setSuffix(suffix);
  box->setKeyboardTracking(false);
  return box;
}

}  // namespace

PeakCenterPanel::PeakCenterPanel(SpectrometerBridge& bridge, QWidget* parent) : QWidget(parent), bridge_(bridge) {
  build();
  set_file({});
  connect(&bridge_, &SpectrometerBridge::peakCenterFinished, this,
          [this](const Result<jobs::PeakCenterResult>& r) { on_finished(r); });
  connect(&bridge_, &SpectrometerBridge::peakCenterNote, this, [this](const QString& note) {
    notes_.append(note);
    show_result();
  });
  connect(&bridge_, &SpectrometerBridge::peakCenterApplied, this, [this](const Result<void>& r) {
    if (r) {
      status_ = tr("Written to field table %1").arg(bridge_.active_table());
    } else {
      applied_ = false;  // nothing was written: Apply may be tried again
      status_ = QString::fromStdString(r.error().what);
    }
    show_result();
    refresh();
  });
  connect(&bridge_, &SpectrometerBridge::fieldTableChanged, this, [this] {
    if (result_) table_changed_ = true;  // the value was measured against another table
    refresh();
  });
  connect(&bridge_, &SpectrometerBridge::peakCenterSetSaved, this, [this](const QString& name, const Result<void>& r) {
    if (!r) {
      status_ = QString::fromStdString(r.error().what);
      show_result();
      return;
    }
    set_file(file_);
    select_set(name);  // load() again: the form is no longer marked edited
    status_ = tr("Saved set %1").arg(name);
    show_result();
  });
}

void PeakCenterPanel::build() {
  auto* outer = new QVBoxLayout(this);

  set_ = new QComboBox;
  set_->setObjectName(QStringLiteral("peakCenterSet"));
  set_->setToolTip(tr("A named set from peak_center.toml. Picking one loads its values into the form."));
  revert_ = new QPushButton(tr("Revert to set"));
  revert_->setObjectName(QStringLiteral("peakCenterRevert"));
  revert_->setToolTip(tr("Discard the edits and load the selected set's values again."));
  auto* top = new QHBoxLayout;
  top->addWidget(new QLabel(tr("Set")));
  top->addWidget(set_, 1);
  top->addWidget(revert_);
  outer->addLayout(top);

  banner_ = new QLabel;
  banner_->setObjectName(QStringLiteral("peakCenterBanner"));
  banner_->setWordWrap(true);
  outer->addWidget(banner_);

  auto* form = new QFormLayout;
  // A field's label carries its control's tooltip, so either one explains it.
  const auto row = [&](const char* key, const QString& text, QWidget* w, const QString& tip) {
    w->setObjectName(QStringLiteral("pc_%1").arg(QLatin1String(key)));
    w->setToolTip(tip);
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("pc_%1_label").arg(QLatin1String(key)));
    label->setToolTip(tip);
    labels_[key] = label;
    form->addRow(label, w);
  };

  detector_ = new QComboBox;
  detector_->addItem(tr("(reference)"), QString());
  for (const auto& d : bridge_.detectors()) detector_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.name));
  row("detector", tr("Detector"), detector_,
      tr("The detector the peak is centered on. (reference) is the spectrometer's reference detector."));

  isotope_ = new QComboBox;
  isotope_->setEditable(true);
  isotope_->setInsertPolicy(QComboBox::NoInsert);
  row("isotope", tr("Isotope"), isotope_,
      tr("The isotope to center. Its field table position is where the sweep starts, unless a center is set."));

  additional_ = new QListWidget;
  additional_->setMaximumHeight(90);
  for (const auto& d : bridge_.detectors()) {
    auto* item = new QListWidgetItem(QString::fromStdString(d.name), additional_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  row("additional_detectors", tr("Additional detectors"), additional_,
      tr("Other detectors read during the sweep and fitted for display. They do not change the result."));

  center_enabled_ = new QCheckBox(tr("Set center"));
  center_enabled_->setObjectName(QStringLiteral("pc_center_enabled"));
  center_enabled_->setToolTip(tr("Start the sweep at Center instead of at the isotope's field table position."));
  center_ = spin(-1e6, 1e6, 9, 0.001);
  row("center", tr("Center"), center_,
      tr("Where the sweep starts, in the magnet's native units. Used only when Set center is checked."));
  form->addRow(QString(), center_enabled_);

  window_ = spin(0, 1e6, 9, 0.001);
  row("window", tr("Window"), window_,
      tr("How far the sweep goes either side of the center, in native units. A retry widens it."));
  step_ = spin(0, 1e6, 9, 0.0001);
  row("step", tr("Step"), step_, tr("The distance between sweep points, in native units. Smaller than Window."));
  percent_ = spin(0, 100, 2, 1);
  row("percent", tr("Percent"), percent_,
      tr("The peak's edges are where the signal falls below (100 - percent)% of its maximum."));
  min_peak_height_ = spin(0, 1e12, 3, 0.5);
  row("min_peak_height", tr("Min peak height"), min_peak_height_,
      tr("The smallest maximum signal taken as a peak."));
  test_peak_flat_ = new QCheckBox;
  row("test_peak_flat", tr("Test peak flat"), test_peak_flat_, tr("Reject a peak whose top is not flat."));
  n_tries_ = new QSpinBox;
  n_tries_->setRange(0, 1000);
  row("n_tries", tr("Tries"), n_tries_,
      tr("How many sweeps before giving up. After a miss, the next sweep goes around the highest point."));
  direction_ = new QComboBox;
  direction_->addItems({QStringLiteral("increase"), QStringLiteral("decrease")});
  row("direction", tr("Direction"), direction_,
      tr("Sweep from low to high magnet position (increase) or from high to low (decrease)."));
  integration_ = spin(0, 1e4, 3, 0.1, tr(" s"));
  row("integration_s", tr("Integration"), integration_,
      tr("Integration time for each point. 0 keeps the spectrometer's current integration."));
  settle_ = spin(0, 1e4, 3, 0.1, tr(" s"));
  row("settle_s", tr("Settle"), settle_, tr("How long to wait after each magnet step before reading."));
  baseline_timeout_ = spin(0, 1e5, 3, 1, tr(" s"));
  row("baseline_timeout_s", tr("Baseline timeout"), baseline_timeout_,
      tr("How long to wait at the end of a sweep for the signal to fall back to baseline."));
  dac_offset_ = spin(-1e6, 1e6, 9, 0.001);
  row("dac_offset", tr("DAC offset"), dac_offset_, tr("Added to the fitted center, in native units."));
  peak_shift_threshold_ = spin(0, 1e6, 9, 0.001);
  row("peak_shift_threshold", tr("Peak shift threshold"), peak_shift_threshold_,
      tr("The largest move from the starting center that is accepted, in native units. 0 means no limit."));
  propagate_ = new QCheckBox;
  row("propagate", tr("Propagate"), propagate_,
      tr("When the result is applied, also update the other detectors' columns of the field table."));
  outer->addLayout(form);

  reason_ = new QLabel;
  reason_->setObjectName(QStringLiteral("peakCenterReason"));
  reason_->setWordWrap(true);
  outer->addWidget(reason_);

  auto* results = new QFormLayout;
  const auto result_row = [&](const char* name, const QString& caption, QLabel*& label) {
    label = new QLabel;
    label->setObjectName(QStringLiteral("peakCenterResult%1").arg(QLatin1String(name)));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    results->addRow(caption, label);
  };
  result_row("Status", tr("Status"), res_status_);
  result_row("Initial", tr("Initial center"), res_initial_);
  result_row("Center", tr("Fitted center"), res_center_);
  result_row("Shift", tr("Shift"), res_shift_);
  result_row("TableValue", tr("Table value"), res_table_);
  result_row("Additional", tr("Additional detectors"), res_additional_);
  result_row("Notes", tr("Notes"), res_notes_);
  outer->addLayout(results);

  start_ = new QPushButton(tr("Start"));
  start_->setObjectName(QStringLiteral("peakCenterStart"));
  cancel_ = new QPushButton(tr("Cancel"));
  cancel_->setObjectName(QStringLiteral("peakCenterCancel"));
  cancel_->setEnabled(false);
  cancel_->setToolTip(tr("Stop the run at its next step. A settle already under way finishes first."));
  apply_ = new QPushButton(tr("Apply to field table"));
  apply_->setObjectName(QStringLiteral("peakCenterApply"));
  apply_->setEnabled(false);
  save_ = new QPushButton(tr("Save to set…"));
  save_->setObjectName(QStringLiteral("peakCenterSave"));
  save_->setToolTip(tr("Write the form's values to a named set in peak_center.toml."));
  auto* buttons = new QHBoxLayout;
  for (auto* b : {start_, cancel_, apply_, save_}) buttons->addWidget(b);
  outer->addLayout(buttons);
  outer->addStretch(1);

  const auto changed = [this] { refresh(); };
  for (auto* w : {window_, step_, percent_, min_peak_height_, integration_, settle_, baseline_timeout_, dac_offset_,
                  peak_shift_threshold_, center_}) {
    connect(w, &QDoubleSpinBox::valueChanged, this, changed);
  }
  connect(n_tries_, &QSpinBox::valueChanged, this, changed);
  for (auto* w : {center_enabled_, test_peak_flat_, propagate_}) connect(w, &QCheckBox::toggled, this, changed);
  connect(direction_, &QComboBox::currentIndexChanged, this, changed);
  connect(isotope_, &QComboBox::currentTextChanged, this, changed);
  connect(additional_, &QListWidget::itemChanged, this, changed);
  connect(detector_, &QComboBox::currentIndexChanged, this, [this] {
    if (!loading_) fill_isotopes(isotope_->currentText());
    refresh();
  });
  connect(set_, &QComboBox::currentIndexChanged, this, [this] {
    const auto it = sets_.find(set_->currentText().toStdString());
    if (!loading_ && it != sets_.end()) load(it->second);
  });
  connect(revert_, &QPushButton::clicked, this, [this] { revert(); });
  connect(start_, &QPushButton::clicked, this, [this] { start(); });
  connect(cancel_, &QPushButton::clicked, this, [this] { bridge_.cancel_peak_center(); });
  connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
  connect(save_, &QPushButton::clicked, this, [this] { save(); });
  show_result();
}

void PeakCenterPanel::set_queue_running(std::function<bool()> queue_running) {
  queue_running_ = std::move(queue_running);
  refresh();
}

bool PeakCenterPanel::confirm(const QString& question) {
  if (ask_) return ask_(question);
  return QMessageBox::question(this, tr("Peak center"), question, QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) == QMessageBox::Yes;
}

void PeakCenterPanel::start() {
  if (bridge_.peak_center_running() || queue_is_running()) {
    refresh();
    return;
  }
  const auto cfg = config();
  if (!jobs::validate_peak_center_config(cfg)) return;
  clear_result();
  emit startRequested(cfg);
  bridge_.peak_center(cfg);
  refresh();
}

void PeakCenterPanel::clear_result() {
  result_.reset();
  status_.clear();
  notes_.clear();
  table_changed_ = false;
  applied_ = false;
  show_result();
}

void PeakCenterPanel::on_finished(const Result<jobs::PeakCenterResult>& result) {
  notes_.clear();
  table_changed_ = false;
  applied_ = false;
  if (result) {
    result_ = *result;
    status_ = QString::fromStdString(result->message);
    if (status_.isEmpty()) status_ = result->ok ? tr("Peak center done") : tr("Peak center failed");
    for (const auto& n : result->notes) notes_.append(QString::fromStdString(n));
  } else {
    result_.reset();
    status_ = QString::fromStdString(result.error().what);
  }
  show_result();
  refresh();
}

void PeakCenterPanel::show_result() {
  const auto number = [](const std::optional<double>& v) { return v ? native(*v) : QString(); };
  res_status_->setText(status_);
  res_initial_->setText(result_ ? native(result_->initial_center) : QString());
  res_center_->setText(result_ ? number(result_->center) : QString());
  res_shift_->setText(result_ && result_->center ? native(*result_->center - result_->initial_center) : QString());
  res_table_->setText(result_ ? number(result_->table_value) : QString());
  QStringList additional;
  if (result_) {
    for (const auto& a : result_->additional) {
      const QString value = a.shape ? native(a.shape->center_x) : QString::fromStdString(a.error);
      additional.append(QStringLiteral("%1: %2").arg(QString::fromStdString(a.detector), value));
    }
  }
  res_additional_->setText(additional.join(QLatin1Char('\n')));
  res_notes_->setText(notes_.join(QLatin1Char('\n')));
}

bool PeakCenterPanel::apply_enabled() const {
  return result_ && result_->ok && result_->table_value && !result_->isotope.empty() && !table_changed_ && !applied_ &&
         !bridge_.peak_center_running() && !queue_is_running();
}

void PeakCenterPanel::apply() {
  if (!apply_enabled()) return;
  const QString isotope = QString::fromStdString(result_->isotope);
  const QString detector = QString::fromStdString(result_->detector);
  const bool propagate = result_->config.propagate;
  const double value = *result_->table_value;
  const QString question = tr("Write %1 for %2 on %3 to %4? Propagate: %5.")
                               .arg(native(value), isotope, detector, bridge_.active_table(),
                                    propagate ? tr("yes") : tr("no"));
  if (!confirm(question)) return;
  applied_ = true;  // until its answer: a second click must not write twice
  refresh();
  bridge_.apply_peak_center(detector, isotope, value, propagate);
}

void PeakCenterPanel::save() {
  if (!file_ok() || file_.empty()) return;
  const QString suggestion = set_name();
  std::optional<QString> asked;
  if (ask_name_) {
    asked = ask_name_(suggestion);
  } else {
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Save to set"), tr("Set name"), QLineEdit::Normal, suggestion, &ok);
    if (ok) asked = text;
  }
  if (!asked) return;
  const QString name = asked->trimmed();
  if (!valid_set_name(name)) {
    reason_->setText(tr("A set name is letters, digits, “_” and “-”."));
    return;
  }
  if (sets_.contains(name.toStdString()) && !confirm(tr("Overwrite set %1?").arg(name))) return;
  auto cfg = config();
  cfg.name = name.toStdString();
  bridge_.save_peak_center_set(file_, name, cfg);
}

void PeakCenterPanel::set_file(const std::filesystem::path& file) {
  file_ = file;
  const QString keep = set_name();
  sets_.clear();
  banner_error_.clear();
  std::string text;
  bool have = false;
  if (!file.empty()) {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      have = true;
    }
  }
  if (have) {
    auto parsed = jobs::parse_peak_center_configs(text, file.string());
    if (parsed) {
      sets_ = std::move(*parsed);
    } else {
      banner_error_ = QString::fromStdString(parsed.error().what);
    }
  }
  if (!sets_.contains("default")) sets_.emplace("default", jobs::PeakCenterConfig{});

  loading_ = true;
  set_->clear();
  for (const auto& entry : sets_) set_->addItem(QString::fromStdString(entry.first));  // sorted by name
  loading_ = false;

  const int at = set_->findText(keep);
  loading_ = true;  // load() below is the one load
  set_->setCurrentIndex(at >= 0 ? at : std::max(0, set_->findText(QStringLiteral("default"))));
  loading_ = false;
  load(sets_.at(set_->currentText().toStdString()));
}

void PeakCenterPanel::select_set(const QString& name) {
  const int at = set_->findText(name);
  if (at < 0) return;
  if (at == set_->currentIndex()) {
    revert();
  } else {
    set_->setCurrentIndex(at);
  }
}

void PeakCenterPanel::revert() {
  const auto it = sets_.find(set_->currentText().toStdString());
  if (it != sets_.end()) load(it->second);
}

QString PeakCenterPanel::set_name() const { return set_ != nullptr ? set_->currentText() : QString(); }

void PeakCenterPanel::fill_isotopes(const QString& keep) {
  const bool was = loading_;
  loading_ = true;
  QString detector = detector_->currentData().toString();
  if (detector.isEmpty()) detector = bridge_.reference_detector();
  isotope_->clear();
  isotope_->addItems(bridge_.isotopes_for(detector));
  isotope_->setCurrentText(keep);
  loading_ = was;
}

void PeakCenterPanel::load(const jobs::PeakCenterConfig& c) {
  loading_ = true;
  const QString detector = QString::fromStdString(c.detector);
  int at = detector_->findData(detector);
  if (at < 0) {
    detector_->addItem(detector, detector);
    at = detector_->count() - 1;
  }
  detector_->setCurrentIndex(at);
  fill_isotopes(QString::fromStdString(c.isotope));
  for (const auto& name : c.additional_detectors) {
    const QString text = QString::fromStdString(name);
    if (additional_->findItems(text, Qt::MatchExactly).isEmpty()) {
      auto* item = new QListWidgetItem(text, additional_);
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    }
  }
  for (int i = 0; i < additional_->count(); ++i) {
    auto* item = additional_->item(i);
    const bool on = std::ranges::find(c.additional_detectors, item->text().toStdString()) != c.additional_detectors.end();
    item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
  }
  center_enabled_->setChecked(c.center.has_value());
  center_->setValue(c.center.value_or(0));
  window_->setValue(c.window);
  step_->setValue(c.step);
  percent_->setValue(c.percent);
  min_peak_height_->setValue(c.min_peak_height);
  test_peak_flat_->setChecked(c.test_peak_flat);
  n_tries_->setValue(c.n_tries);
  direction_->setCurrentIndex(c.increase ? 0 : 1);
  integration_->setValue(seconds(c.integration));
  settle_->setValue(seconds(c.settle));
  baseline_timeout_->setValue(seconds(c.baseline_timeout));
  dac_offset_->setValue(c.dac_offset);
  peak_shift_threshold_->setValue(c.peak_shift_threshold);
  propagate_->setChecked(c.propagate);
  exact_ = c;
  loading_ = false;
  base_ = form_config();
  refresh();
}

jobs::PeakCenterConfig PeakCenterPanel::config() const {
  const auto form = form_config();
  auto c = exact_;  // a field the operator has not edited keeps the set's exact value
  c.name = form.name;
#define PC_TAKE(f) \
  if (form.f != base_.f) c.f = form.f
  PC_TAKE(detector);
  PC_TAKE(isotope);
  PC_TAKE(additional_detectors);
  PC_TAKE(center);
  PC_TAKE(window);
  PC_TAKE(step);
  PC_TAKE(percent);
  PC_TAKE(min_peak_height);
  PC_TAKE(test_peak_flat);
  PC_TAKE(n_tries);
  PC_TAKE(increase);
  PC_TAKE(integration);
  PC_TAKE(settle);
  PC_TAKE(baseline_timeout);
  PC_TAKE(dac_offset);
  PC_TAKE(peak_shift_threshold);
  PC_TAKE(propagate);
#undef PC_TAKE
  return c;
}

jobs::PeakCenterConfig PeakCenterPanel::form_config() const {
  jobs::PeakCenterConfig c;
  c.name = set_name().toStdString();
  c.detector = detector_->currentData().toString().toStdString();
  c.isotope = isotope_->currentText().toStdString();
  for (int i = 0; i < additional_->count(); ++i) {
    if (additional_->item(i)->checkState() == Qt::Checked) c.additional_detectors.push_back(additional_->item(i)->text().toStdString());
  }
  if (center_enabled_->isChecked()) c.center = center_->value();
  c.window = window_->value();
  c.step = step_->value();
  c.percent = percent_->value();
  c.min_peak_height = min_peak_height_->value();
  c.test_peak_flat = test_peak_flat_->isChecked();
  c.n_tries = n_tries_->value();
  c.increase = direction_->currentIndex() == 0;
  c.integration = duration_of(integration_->value());
  c.settle = duration_of(settle_->value());
  c.baseline_timeout = duration_of(baseline_timeout_->value());
  c.dac_offset = dac_offset_->value();
  c.peak_shift_threshold = peak_shift_threshold_->value();
  c.propagate = propagate_->isChecked();
  return c;
}

bool PeakCenterPanel::edited() const {
  const auto now = form_config();
  return now.detector != base_.detector || now.isotope != base_.isotope ||
         now.additional_detectors != base_.additional_detectors || now.center != base_.center ||
         now.window != base_.window || now.step != base_.step || now.percent != base_.percent ||
         now.min_peak_height != base_.min_peak_height || now.test_peak_flat != base_.test_peak_flat ||
         now.n_tries != base_.n_tries || now.increase != base_.increase || now.integration != base_.integration ||
         now.settle != base_.settle || now.baseline_timeout != base_.baseline_timeout ||
         now.dac_offset != base_.dac_offset || now.peak_shift_threshold != base_.peak_shift_threshold ||
         now.propagate != base_.propagate;
}

void PeakCenterPanel::refresh() {
  if (loading_) return;
  const auto now = form_config();
  const auto mark = [this](const char* key, bool differs) {
    QLabel* label = labels_.at(key);
    QFont f = label->font();
    f.setBold(differs);
    label->setFont(f);
  };
  mark("detector", now.detector != base_.detector);
  mark("isotope", now.isotope != base_.isotope);
  mark("additional_detectors", now.additional_detectors != base_.additional_detectors);
  mark("center", now.center != base_.center);
  mark("window", now.window != base_.window);
  mark("step", now.step != base_.step);
  mark("percent", now.percent != base_.percent);
  mark("min_peak_height", now.min_peak_height != base_.min_peak_height);
  mark("test_peak_flat", now.test_peak_flat != base_.test_peak_flat);
  mark("n_tries", now.n_tries != base_.n_tries);
  mark("direction", now.increase != base_.increase);
  mark("integration_s", now.integration != base_.integration);
  mark("settle_s", now.settle != base_.settle);
  mark("baseline_timeout_s", now.baseline_timeout != base_.baseline_timeout);
  mark("dac_offset", now.dac_offset != base_.dac_offset);
  mark("peak_shift_threshold", now.peak_shift_threshold != base_.peak_shift_threshold);
  mark("propagate", now.propagate != base_.propagate);

  banner_->setText(banner_error_);
  banner_->setVisible(!banner_error_.isEmpty());
  const auto valid = jobs::validate_peak_center_config(config());
  reason_->setText(valid ? QString() : QString::fromStdString(valid.error().what));
  const bool running = bridge_.peak_center_running();
  const bool queue = queue_is_running();
  if (queue && result_) table_changed_ = true;
  start_->setEnabled(valid && !running && !queue);
  start_->setToolTip(queue ? tr("a queue is running") : tr("Run a peak center with the values in the form."));
  cancel_->setEnabled(running);
  apply_->setEnabled(apply_enabled());
  apply_->setToolTip(queue ? tr("a queue is running")
                           : tr("Write the fitted value to the active field table. Asks first."));
  save_->setEnabled(valid && file_ok());
  revert_->setEnabled(edited());
}

}  // namespace pychron::ui
