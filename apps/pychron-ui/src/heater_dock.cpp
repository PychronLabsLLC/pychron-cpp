#include "heater_dock.hpp"

#include <QDoubleValidator>
#include <QGridLayout>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace pychron::ui {

namespace {

std::vector<DetectorSeries> series_for(const config::SystemConfig& config) {
  std::vector<DetectorSeries> out;
  for (std::size_t i = 0; i < config.heaters.size(); ++i) {
    const auto& h = config.heaters[i];
    out.push_back({h.name, StripChartModel::palette_color(i), QString::fromStdString(h.units), {}, true});
  }
  return out;
}

}  // namespace

HeaterDock::HeaterDock(CoreBridge& bridge, QWidget* parent)
    : QDockWidget(QStringLiteral("Heaters"), parent),
      bridge_(bridge),
      model_(std::make_unique<StripChartModel>(series_for(bridge.config()))) {
  setObjectName(QStringLiteral("heater_dock"));
  confirm_ = [this](const QString& question) {
    return QMessageBox::question(this, QStringLiteral("Heater"), question) == QMessageBox::Yes;
  };
  auto* body = new QWidget(this);
  auto* layout = new QVBoxLayout(body);
  auto* grid = new QGridLayout;
  layout->addLayout(grid);

  int r = 0;
  for (const auto& h : bridge_.config().heaters) {
    const std::string heater = h.name;
    const QString units = QString::fromStdString(h.units);
    Row row;
    auto* name = new QLabel(QString::fromStdString(h.name), body);
    name->setToolTip(QString::fromStdString(h.description));
    name->setStyleSheet(QStringLiteral("color: %1").arg(StripChartModel::palette_color(static_cast<std::size_t>(r)).name()));
    row.power = new QPushButton(QStringLiteral("Off"), body);
    row.power->setCheckable(true);
    row.power->setToolTip(QStringLiteral("Switch the heater on or off"));
    row.pid = new QCheckBox(QStringLiteral("Use PID"), body);
    row.setpoint = new QLineEdit(body);
    row.setpoint->setValidator(new QDoubleValidator(row.setpoint));
    row.setpoint->setPlaceholderText(QStringLiteral("setpoint"));
    row.setpoint->setToolTip(QStringLiteral("Press Enter to send"));
    row.readback = new QLCDNumber(7, body);
    row.readback->setSegmentStyle(QLCDNumber::Flat);
    row.readback->setToolTip(QStringLiteral("Readback") + (units.isEmpty() ? QString() : QStringLiteral(" (") + units + ')'));
    row.readback->display(QStringLiteral("-"));
    grid->addWidget(name, r, 0);
    grid->addWidget(row.power, r, 1);
    grid->addWidget(row.pid, r, 2);
    grid->addWidget(row.setpoint, r, 3);
    grid->addWidget(new QLabel(units, body), r, 4);
    grid->addWidget(row.readback, r, 5);

    connect(row.power, &QPushButton::clicked, this, [this, heater] { on_power(heater); });
    connect(row.pid, &QCheckBox::clicked, this, [this, heater](bool on) {
      Row& rw = rows_.at(heater);
      set_pending(rw, true);
      bridge_.set_heater_pid(QString::fromStdString(heater), on);
    });
    connect(row.setpoint, &QLineEdit::returnPressed, this, [this, heater] {
      Row& rw = rows_.at(heater);
      bool ok = false;
      const double value = rw.setpoint->locale().toDouble(rw.setpoint->text(), &ok);
      if (!ok) return;
      set_pending(rw, true);
      bridge_.set_heater_setpoint(QString::fromStdString(heater), value);
    });
    rows_[heater] = row;
    if (auto it = bridge_.state().heaters.find(heater); it != bridge_.state().heaters.end()) show_sample(it->second);
    ++r;
  }

  status_ = new QLabel(body);
  status_->setWordWrap(true);
  layout->addWidget(status_);
  chart_ = new StripChartView(*model_, body);
  chart_->set_y_label(QStringLiteral("Readback"));
  chart_->setMinimumHeight(160);
  layout->addWidget(chart_, 1);
  setWidget(body);

  connect(&bridge_, &CoreBridge::heaterSample, this, [this](const HeaterSample& s) {
    show_sample(s);
    if (s.readback && rows_.contains(s.heater)) {
      spectrometer::IntensityReading reading;
      reading.reading.ts = s.ts;
      reading.reading.values[s.heater] = spectrometer::Value{*s.readback, std::nullopt, std::nullopt, false};
      model_->append(reading);
      chart_->refresh();
    }
  });
  connect(&bridge_, &CoreBridge::heaterCommandFinished, this, &HeaterDock::on_finished);
}

void HeaterDock::show_sample(const HeaterSample& s) {
  auto it = rows_.find(s.heater);
  if (it == rows_.end()) return;
  Row& row = it->second;
  {
    const QSignalBlocker block(row.power);
    row.power->setChecked(s.enabled.value_or(false));
    row.power->setText(s.enabled.value_or(false) ? QStringLiteral("On") : QStringLiteral("Off"));
  }
  {
    const QSignalBlocker block(row.pid);
    row.pid->setChecked(s.use_pid.value_or(false));
  }
  // The field is the operator's while it has focus or text of theirs.
  if (s.setpoint && !row.setpoint->hasFocus() && !row.setpoint->isModified()) {
    row.setpoint->setText(QString::number(*s.setpoint, 'f', 2));
  }
  if (s.readback) row.readback->display(QString::number(*s.readback, 'f', 2));
  row.power->setProperty("supported", s.enabled.has_value());
  row.pid->setProperty("supported", s.use_pid.has_value());
  row.setpoint->setProperty("supported", s.setpoint.has_value());
  set_pending(row, row.pending);
}

void HeaterDock::on_power(const std::string& heater) {
  Row& row = rows_.at(heater);
  // The button shows the heater's state, not the click, until it answers.
  const bool on = row.power->isChecked();
  {
    const QSignalBlocker block(row.power);
    row.power->setChecked(!on);
  }
  const QString name = QString::fromStdString(heater);
  if (!confirm_(QStringLiteral("Turn %1 %2?").arg(name, on ? QStringLiteral("on") : QStringLiteral("off")))) return;
  set_pending(row, true);
  bridge_.set_heater_enabled(name, on);
}

void HeaterDock::on_finished(const QString& heater, const Result<void>& result) {
  auto it = rows_.find(heater.toStdString());
  if (it == rows_.end()) return;
  Row& row = it->second;
  set_pending(row, false);
  if (result) {
    status_->clear();
    row.setpoint->setModified(false);
    return;
  }
  const QString message = heater + QStringLiteral(": ") + QString::fromStdString(to_string(result.error()));
  status_->setText(message);
  // Back to what the heater last said.
  if (auto s = bridge_.state().heaters.find(heater.toStdString()); s != bridge_.state().heaters.end()) {
    row.setpoint->setModified(false);
    show_sample(s->second);
  }
  emit heaterFailed(message);
}

void HeaterDock::set_pending(Row& row, bool pending) {
  row.pending = pending;
  // Before the first sample nothing is known to be unsupported.
  for (QWidget* w : {static_cast<QWidget*>(row.power), static_cast<QWidget*>(row.pid),
                     static_cast<QWidget*>(row.setpoint)}) {
    const QVariant supported = w->property("supported");
    w->setEnabled(!pending && (!supported.isValid() || supported.toBool()));
  }
}

const HeaterDock::Row* HeaterDock::row(const std::string& heater) const {
  auto it = rows_.find(heater);
  return it == rows_.end() ? nullptr : &it->second;
}

QPushButton* HeaterDock::power_button(const std::string& heater) const {
  const Row* r = row(heater);
  return r ? r->power : nullptr;
}

QCheckBox* HeaterDock::pid_box(const std::string& heater) const {
  const Row* r = row(heater);
  return r ? r->pid : nullptr;
}

QLineEdit* HeaterDock::setpoint_field(const std::string& heater) const {
  const Row* r = row(heater);
  return r ? r->setpoint : nullptr;
}

QLCDNumber* HeaterDock::readback(const std::string& heater) const {
  const Row* r = row(heater);
  return r ? r->readback : nullptr;
}

QString HeaterDock::status_text() const { return status_->text(); }

}  // namespace pychron::ui
