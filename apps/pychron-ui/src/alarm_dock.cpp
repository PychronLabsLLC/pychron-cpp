#include "alarm_dock.hpp"

#include <QHBoxLayout>
#include <QTime>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

enum Column { kSource, kSeverity, kMessage, kTime };

QString severity_name(AlarmSeverity severity) {
  switch (severity) {
    case AlarmSeverity::Info:
      return QStringLiteral("info");
    case AlarmSeverity::Warning:
      return QStringLiteral("warning");
    case AlarmSeverity::Critical:
      return QStringLiteral("critical");
  }
  return QStringLiteral("?");
}

}  // namespace

AlarmDock::AlarmDock(QWidget* parent)
    : QDockWidget(tr("Alarms"), parent),
      tree_(new QTreeWidget),
      ack_(new QPushButton(tr("Acknowledge"))),
      ack_all_(new QPushButton(tr("Acknowledge all"))) {
  setObjectName(QStringLiteral("AlarmDock"));
  tree_->setHeaderLabels({tr("Source"), tr("Severity"), tr("Message"), tr("Time")});
  tree_->setRootIsDecorated(false);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  buttons->addWidget(ack_);
  buttons->addWidget(ack_all_);

  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(tree_);
  layout->addLayout(buttons);
  setWidget(body);

  connect(ack_, &QPushButton::clicked, this, [this] { acknowledge_selected(); });
  connect(ack_all_, &QPushButton::clicked, this, &AlarmDock::acknowledge_all);
}

void AlarmDock::add_alarm(const Alarm& alarm) {
  const QString source = QString::fromStdString(alarm.source);
  QTreeWidgetItem* row = row_for(source);
  if (!row) {
    row = new QTreeWidgetItem(tree_);
    row->setText(kSource, source);
  }
  row->setText(kSeverity, severity_name(alarm.severity));
  row->setText(kMessage, QString::fromStdString(alarm.message));
  row->setText(kTime, QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
  const QColor color = alarm.severity == AlarmSeverity::Critical ? QColor(Qt::red) : QColor(0xc0, 0x80, 0x00);
  row->setForeground(kSeverity, color);
}

int AlarmDock::acknowledge_selected() {
  const auto selected = tree_->selectedItems();
  for (QTreeWidgetItem* row : selected) {
    delete row;
  }
  return static_cast<int>(selected.size());
}

void AlarmDock::acknowledge_all() { tree_->clear(); }

int AlarmDock::active_count() const { return tree_->topLevelItemCount(); }

bool AlarmDock::is_active(const std::string& source) const {
  return row_for(QString::fromStdString(source)) != nullptr;
}

QTreeWidgetItem* AlarmDock::row_for(const QString& source) const {
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    if (tree_->topLevelItem(i)->text(kSource) == source) {
      return tree_->topLevelItem(i);
    }
  }
  return nullptr;
}

}  // namespace pychron::ui
