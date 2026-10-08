#include "metrics_page.hpp"

#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include "preferences_dialog.hpp"
#include "pychron/core/config/loader.hpp"

namespace pychron::ui {

namespace {
const char* const kLoopback = "127.0.0.1";
const char* const kEverywhere = "0.0.0.0";
}  // namespace

MetricsPage::MetricsPage(QWidget* parent)
    : QWidget(parent),
      enabled_(new QCheckBox(tr("Publish metrics for the lab's monitoring box"))),
      where_(new QComboBox),
      address_(new QLineEdit),
      port_(new QSpinBox),
      status_(new QLabel) {
  where_->addItem(tr("This computer only"));
  where_->addItem(tr("Every address of this computer"));
  where_->addItem(tr("One address:"));
  address_->setPlaceholderText(tr("192.168.1.20"));
  address_->setEnabled(false);
  connect(where_, &QComboBox::currentIndexChanged, this, [this](int index) { address_->setEnabled(index == OneAddress); });
  port_->setRange(1, 65535);
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* form = new QFormLayout(this);
  form->addRow(tr("Now:"), status_);
  form->addRow(enabled_);
  form->addRow(tr("Reachable from:"), where_);
  form->addRow(QString(), address_);
  form->addRow(tr("Port:"), port_);
  form->addRow(preferences_note(
      tr("These take effect the next time pychron starts. The numbers are read-only and hold no sample, project "
         "or user name, but anyone who can reach the port can read them: there is no password. The monitoring box "
         "needs more than \"this computer only\".")));
}

void MetricsPage::set(const config::MetricsConfig& metrics, const QString& status) {
  base_ = metrics;
  enabled_->setChecked(metrics.enabled);
  port_->setValue(static_cast<int>(metrics.port));
  if (metrics.bind == kLoopback) {
    where_->setCurrentIndex(ThisComputer);
    address_->clear();
  } else if (metrics.bind == kEverywhere) {
    where_->setCurrentIndex(AllAddresses);
    address_->clear();
  } else {
    where_->setCurrentIndex(OneAddress);
    address_->setText(QString::fromStdString(metrics.bind));
  }
  address_->setEnabled(where_->currentIndex() == OneAddress);
  status_->setText(status.isEmpty() ? tr("Not known") : status);
}

config::MetricsConfig MetricsPage::value() const {
  config::MetricsConfig out = base_;
  out.enabled = enabled_->isChecked();
  out.port = port_->value();
  switch (where_->currentIndex()) {
    case ThisComputer: out.bind = kLoopback; break;
    case AllAddresses: out.bind = kEverywhere; break;
    default: out.bind = address_->text().trimmed().toStdString(); break;
  }
  return out;
}

QString MetricsPage::problem() const {
  if (where_->currentIndex() != OneAddress) return {};
  const QString text = address_->text().trimmed();
  if (text.isEmpty()) return tr("Metrics: give the address to publish on, or choose another \"Reachable from\".");
  if (!config::is_ip_address(text.toStdString())) {
    return tr("Metrics: \"%1\" is not an address. Give one of this computer's addresses in numbers, "
              "such as 192.168.1.20; a name will not do.")
        .arg(text);
  }
  return {};
}

}  // namespace pychron::ui
