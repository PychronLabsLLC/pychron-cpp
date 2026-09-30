#include "health_bar.hpp"

#include <algorithm>
#include <chrono>

namespace pychron::ui {

namespace {

QString age_text(std::chrono::seconds age) {
  const auto s = age.count();
  if (s < 60) {
    return QStringLiteral("%1s").arg(s);
  }
  if (s < 3600) {
    return QStringLiteral("%1m").arg(s / 60);
  }
  return QStringLiteral("%1h").arg(s / 3600);
}

}  // namespace

HealthBar::HealthBar(QWidget* parent, std::function<TimePoint()> now)
    : QWidget(parent), now_(std::move(now)), layout_(new QHBoxLayout(this)) {
  if (!now_) {
    now_ = [] { return std::chrono::steady_clock::now(); };
  }
  layout_->setContentsMargins(4, 0, 4, 0);
  layout_->setSpacing(6);
  ticker_.setInterval(1000);
  connect(&ticker_, &QTimer::timeout, this, &HealthBar::refresh);
  ticker_.start();
}

void HealthBar::seed(const std::vector<std::string>& transports) {
  for (const auto& name : transports) {
    render(name, chip_for(name));
  }
}

void HealthBar::update_health(const TransportHealth& health) {
  Chip& chip = chip_for(health.transport);
  chip.status = status_of(health);
  if (health.connected) {
    chip.last_ok = health.ts;
  }
  QString tip = QStringLiteral("%1: %2 error(s)").arg(QString::fromStdString(health.transport)).arg(health.error_count);
  if (!health.last_error.empty()) {
    tip += QStringLiteral("\nlast error: ") + QString::fromStdString(health.last_error);
  }
  chip.label->setToolTip(tip);
  render(health.transport, chip);
}

void HealthBar::refresh() {
  for (const auto& [name, chip] : chips_) {
    render(name, chip);
  }
}

HealthBar::Status HealthBar::status_of(const TransportHealth& health) {
  if (!health.connected) {
    return Status::Down;
  }
  return health.error_count == 0 ? Status::Ok : Status::Degraded;
}

QColor HealthBar::color_of(Status status) {
  switch (status) {
    case Status::Ok:
      return QColor(0x2e, 0xcc, 0x40);
    case Status::Degraded:
      return QColor(0xff, 0xb3, 0x00);
    case Status::Down:
      return QColor(0xe0, 0x3c, 0x31);
    case Status::Unknown:
      break;
  }
  return QColor(0xaa, 0xaa, 0xaa);
}

std::optional<HealthBar::Status> HealthBar::status(const std::string& transport) const {
  auto it = chips_.find(transport);
  if (it == chips_.end()) {
    return std::nullopt;
  }
  return it->second.status;
}

QLabel* HealthBar::chip(const std::string& transport) const {
  auto it = chips_.find(transport);
  return it == chips_.end() ? nullptr : it->second.label;
}

HealthBar::Chip& HealthBar::chip_for(const std::string& transport) {
  Chip& chip = chips_[transport];
  if (!chip.label) {
    chip.label = new QLabel(this);
    chip.label->setObjectName(QString::fromStdString(transport));
    chip.label->setMargin(2);
    layout_->addWidget(chip.label);
  }
  return chip;
}

void HealthBar::render(const std::string& name, const Chip& chip) {
  QString text = QString::fromStdString(name);
  if (chip.last_ok) {
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now_() - *chip.last_ok);
    text += QStringLiteral(" · ") + age_text(std::max(age, std::chrono::seconds{0}));
  }
  chip.label->setText(text);
  chip.label->setStyleSheet(QStringLiteral("QLabel { background: %1; border-radius: 4px; padding: 1px 6px; }")
                                .arg(color_of(chip.status).name()));
}

}  // namespace pychron::ui
