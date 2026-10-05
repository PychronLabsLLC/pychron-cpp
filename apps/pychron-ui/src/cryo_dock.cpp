#include "cryo_dock.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

namespace pychron::ui {

namespace {

QString kelvin_text(double kelvin) { return QStringLiteral("%1 K").arg(kelvin, 0, 'f', 2); }

std::vector<DetectorSeries> series_for(const std::vector<std::string>& inputs) {
  std::vector<DetectorSeries> out;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    out.push_back({inputs[i], StripChartModel::palette_color(i), QStringLiteral("K"), {}, true});
  }
  return out;
}

}  // namespace

CryoDock::CryoDock(CoreBridge& bridge, QWidget* parent)
    : QDockWidget(QStringLiteral("Cryo"), parent),
      bridge_(bridge),
      model_(std::make_unique<StripChartModel>(series_for(bridge.cryo_inputs()))) {
  setObjectName(QStringLiteral("cryo_dock"));
  auto* body = new QWidget(this);
  auto* layout = new QVBoxLayout(body);
  auto* form = new QFormLayout;
  layout->addLayout(form);

  const auto inputs = bridge_.cryo_inputs();
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = inputs[i];
    auto* label = new QLabel(QStringLiteral("—"), body);
    label->setObjectName(QStringLiteral("cryo_input_") + QString::fromStdString(input));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (auto it = bridge_.state().temperatures.find(input); it != bridge_.state().temperatures.end()) {
      label->setText(kelvin_text(it->second));
    }
    auto* name = new QLabel(QStringLiteral("Input %1").arg(QString::fromStdString(input)), body);
    name->setStyleSheet(QStringLiteral("color: %1").arg(StripChartModel::palette_color(i).name()));
    form->addRow(name, label);
    temperatures_[input] = label;
  }

  for (int output = 1; output <= bridge_.cryo_outputs(); ++output) {
    Loop loop;
    auto* row = new QHBoxLayout;
    loop.field = new QDoubleSpinBox(body);
    loop.field->setRange(0.0, 1000.0);
    loop.field->setDecimals(2);
    loop.field->setSuffix(QStringLiteral(" K"));
    loop.set = new QPushButton(QStringLiteral("Set"), body);
    loop.readback = new QLabel(QStringLiteral("—"), body);
    loop.readback->setToolTip(QStringLiteral("The setpoint the controller reports"));
    row->addWidget(loop.field, 1);
    row->addWidget(loop.set);
    row->addWidget(loop.readback);
    form->addRow(QStringLiteral("Setpoint %1").arg(output), row);
    connect(loop.set, &QPushButton::clicked, this, [this, output] { apply_setpoint(output); });
    loops_[output] = loop;
  }

  status_ = new QLabel(body);
  status_->setWordWrap(true);
  layout->addWidget(status_);

  chart_ = new StripChartView(*model_, body);
  chart_->set_y_label(QStringLiteral("Temperature (K)"));
  chart_->setMinimumHeight(160);
  layout->addWidget(chart_, 1);
  setWidget(body);

  connect(&bridge_, &CoreBridge::temperatureSample, this, &CryoDock::on_sample);
  connect(&bridge_, &CoreBridge::cryoSetpoint, this, &CryoDock::on_setpoint);
  connect(&bridge_, &CoreBridge::snapshot, this, [this] { read_setpoints(); });
  if (bridge_.state().started) read_setpoints();
}

void CryoDock::read_setpoints() {
  for (const auto& [output, loop] : loops_) bridge_.read_cryo_setpoint(output);
}

void CryoDock::apply_setpoint(int output) {
  auto it = loops_.find(output);
  if (it == loops_.end()) return;
  it->second.set->setEnabled(false);  // until the controller answers
  bridge_.set_cryo_setpoint(output, it->second.field->value());
}

void CryoDock::on_sample(const TemperatureSample& sample) {
  auto it = temperatures_.find(sample.input);
  if (it == temperatures_.end()) return;
  it->second->setText(kelvin_text(sample.kelvin));
  spectrometer::IntensityReading row;
  row.reading.ts = sample.ts;
  row.reading.values[sample.input] = spectrometer::Value{sample.kelvin, std::nullopt, std::nullopt, false};
  model_->append(row);
  chart_->refresh();
}

void CryoDock::on_setpoint(int output, bool set, const Result<double>& setpoint) {
  auto it = loops_.find(output);
  if (it == loops_.end()) return;
  Loop& loop = it->second;
  if (set) loop.set->setEnabled(true);
  if (!setpoint) {
    const QString message = QStringLiteral("setpoint %1 %2: %3")
                                .arg(output)
                                .arg(set ? QStringLiteral("not set") : QStringLiteral("not read"),
                                     QString::fromStdString(to_string(setpoint.error())));
    status_->setText(message);
    emit cryoFailed(message);
    return;
  }
  status_->clear();
  loop.readback->setText(kelvin_text(*setpoint));
  // The field starts at what the controller holds; after that it is the user's.
  if (!set && !loop.field->property("seeded").toBool()) {
    loop.field->setValue(*setpoint);
    loop.field->setProperty("seeded", true);
  }
}

QString CryoDock::temperature_text(const std::string& input) const {
  auto it = temperatures_.find(input);
  return it == temperatures_.end() ? QString() : it->second->text();
}

QString CryoDock::setpoint_text(int output) const {
  auto it = loops_.find(output);
  return it == loops_.end() ? QString() : it->second.readback->text();
}

QDoubleSpinBox* CryoDock::setpoint_field(int output) const {
  auto it = loops_.find(output);
  return it == loops_.end() ? nullptr : it->second.field;
}

QPushButton* CryoDock::set_button(int output) const {
  auto it = loops_.find(output);
  return it == loops_.end() ? nullptr : it->second.set;
}

QString CryoDock::status_text() const { return status_->text(); }

}  // namespace pychron::ui
