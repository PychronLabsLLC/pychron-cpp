#include "peak_center_panel.hpp"

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
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "spectrometer_bridge.hpp"

namespace pychron::ui {

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }
Duration duration_of(double s) { return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(s)); }

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
}

void PeakCenterPanel::build() {
  auto* outer = new QVBoxLayout(this);

  set_ = new QComboBox;
  set_->setObjectName(QStringLiteral("peakCenterSet"));
  revert_ = new QPushButton(tr("Revert to set"));
  revert_->setObjectName(QStringLiteral("peakCenterRevert"));
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
  const auto row = [&](const char* key, const QString& text, QWidget* w) {
    w->setObjectName(QStringLiteral("pc_%1").arg(QLatin1String(key)));
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("pc_%1_label").arg(QLatin1String(key)));
    labels_[key] = label;
    form->addRow(label, w);
  };

  detector_ = new QComboBox;
  detector_->addItem(tr("(reference)"), QString());
  for (const auto& d : bridge_.detectors()) detector_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.name));
  row("detector", tr("Detector"), detector_);

  isotope_ = new QComboBox;
  isotope_->setEditable(true);
  isotope_->setInsertPolicy(QComboBox::NoInsert);
  row("isotope", tr("Isotope"), isotope_);

  additional_ = new QListWidget;
  additional_->setMaximumHeight(90);
  for (const auto& d : bridge_.detectors()) {
    auto* item = new QListWidgetItem(QString::fromStdString(d.name), additional_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  row("additional_detectors", tr("Additional detectors"), additional_);

  center_enabled_ = new QCheckBox(tr("Set center"));
  center_enabled_->setObjectName(QStringLiteral("pc_center_enabled"));
  center_ = spin(-1e6, 1e6, 6, 0.001);
  row("center", tr("Center"), center_);
  form->addRow(QString(), center_enabled_);

  window_ = spin(0, 1e6, 6, 0.001);
  row("window", tr("Window"), window_);
  step_ = spin(0, 1e6, 6, 0.0001);
  row("step", tr("Step"), step_);
  percent_ = spin(0, 100, 2, 1);
  row("percent", tr("Percent"), percent_);
  min_peak_height_ = spin(0, 1e12, 3, 0.5);
  row("min_peak_height", tr("Min peak height"), min_peak_height_);
  test_peak_flat_ = new QCheckBox;
  row("test_peak_flat", tr("Test peak flat"), test_peak_flat_);
  n_tries_ = new QSpinBox;
  n_tries_->setRange(0, 1000);
  row("n_tries", tr("Tries"), n_tries_);
  direction_ = new QComboBox;
  direction_->addItems({QStringLiteral("increase"), QStringLiteral("decrease")});
  row("direction", tr("Direction"), direction_);
  integration_ = spin(0, 1e4, 3, 0.1, tr(" s"));
  row("integration_s", tr("Integration"), integration_);
  settle_ = spin(0, 1e4, 3, 0.1, tr(" s"));
  row("settle_s", tr("Settle"), settle_);
  baseline_timeout_ = spin(0, 1e5, 3, 1, tr(" s"));
  row("baseline_timeout_s", tr("Baseline timeout"), baseline_timeout_);
  dac_offset_ = spin(-1e6, 1e6, 6, 0.001);
  row("dac_offset", tr("DAC offset"), dac_offset_);
  peak_shift_threshold_ = spin(0, 1e6, 6, 0.001);
  row("peak_shift_threshold", tr("Peak shift threshold"), peak_shift_threshold_);
  propagate_ = new QCheckBox;
  row("propagate", tr("Propagate"), propagate_);
  outer->addLayout(form);

  reason_ = new QLabel;
  reason_->setObjectName(QStringLiteral("peakCenterReason"));
  reason_->setWordWrap(true);
  outer->addWidget(reason_);

  start_ = new QPushButton(tr("Start"));
  start_->setObjectName(QStringLiteral("peakCenterStart"));
  cancel_ = new QPushButton(tr("Cancel"));
  cancel_->setObjectName(QStringLiteral("peakCenterCancel"));
  cancel_->setEnabled(false);
  apply_ = new QPushButton(tr("Apply to field table"));
  apply_->setObjectName(QStringLiteral("peakCenterApply"));
  apply_->setEnabled(false);
  save_ = new QPushButton(tr("Save to set…"));
  save_->setObjectName(QStringLiteral("peakCenterSave"));
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
  connect(start_, &QPushButton::clicked, this, [this] { emit startRequested(config()); });
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
  set_->setCurrentIndex(at >= 0 ? at : std::max(0, set_->findText(QStringLiteral("default"))));
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
  update_table_ = c.update_table;
  loading_ = false;
  base_ = config();
  refresh();
}

jobs::PeakCenterConfig PeakCenterPanel::config() const {
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
  c.update_table = update_table_;
  c.propagate = propagate_->isChecked();
  return c;
}

bool PeakCenterPanel::edited() const {
  const auto now = config();
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
  const auto now = config();
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
  const auto valid = jobs::validate_peak_center_config(now);
  reason_->setText(valid ? QString() : QString::fromStdString(valid.error().what));
  start_->setEnabled(static_cast<bool>(valid));
  save_->setEnabled(valid && file_ok());
  revert_->setEnabled(edited());
}

}  // namespace pychron::ui
