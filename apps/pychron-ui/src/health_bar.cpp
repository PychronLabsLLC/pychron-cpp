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

QString countdown_text(std::chrono::seconds left) {
  const auto s = std::max<long long>(left.count(), 0);
  return QStringLiteral("%1:%2:%3")
      .arg(s / 3600)
      .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(s % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

HealthBar::HealthBar(QWidget* parent, std::function<TimePoint()> now)
    : QWidget(parent), now_(std::move(now)), layout_(new QHBoxLayout(this)), hold_chip_(new QLabel(this)) {
  if (!now_) {
    now_ = [] { return std::chrono::steady_clock::now(); };
  }
  layout_->setContentsMargins(4, 0, 4, 0);
  layout_->setSpacing(6);
  hold_chip_->setObjectName(QStringLiteral("HoldChip"));
  hold_chip_->setMargin(2);
  hold_chip_->setToolTip(tr("The queue holds: it starts the next run once the links it needs are back, "
                            "and ends when the count reaches zero (Experiment > Executor: Resume, Skip Held Run)"));
  hold_chip_->hide();
  layout_->addWidget(hold_chip_);
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
  render_hold();
}

void HealthBar::show_hold(const QString& reason, TimePoint until) {
  hold_ = Hold{reason, until};
  render_hold();
}

void HealthBar::clear_hold() {
  hold_.reset();
  render_hold();
}

QString HealthBar::hold_text_at(TimePoint now) const {
  if (!hold_) {
    return {};
  }
  // Rounded up: a hold with 0.4 s left still has time, and says so.
  const auto left = std::chrono::ceil<std::chrono::seconds>(hold_->until - now);
  return tr("holding: %1 · ends in %2").arg(hold_->reason, countdown_text(left));
}

QString HealthBar::hold_text() const { return hold_chip_->isHidden() ? QString() : hold_chip_->text(); }

void HealthBar::render_hold() {
  if (!hold_) {
    hold_chip_->clear();
    hold_chip_->hide();
    return;
  }
  hold_chip_->setText(hold_text_at(now_()));
  style::set_chip(hold_chip_, style::Level::Warning);
  hold_chip_->show();
}

HealthBar::Status HealthBar::status_of(const TransportHealth& health) {
  if (!health.connected) {
    return Status::Down;
  }
  return health.error_count == 0 ? Status::Ok : Status::Degraded;
}

style::Level HealthBar::level_of(Status status) {
  switch (status) {
    case Status::Ok:
      return style::Level::Ok;
    case Status::Degraded:
      return style::Level::Warning;
    case Status::Down:
      return style::Level::Error;
    case Status::Unknown:
      break;
  }
  return style::Level::Unknown;
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
  style::set_chip(chip.label, level_of(chip.status));
}

}  // namespace pychron::ui
