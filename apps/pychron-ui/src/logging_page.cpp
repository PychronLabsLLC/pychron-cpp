#include "logging_page.hpp"

#include <map>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "preferences_dialog.hpp"

namespace pychron::ui {

namespace {

constexpr LogLevel kLevels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error};
constexpr const char* kLevelNames[] = {"trace", "debug", "info", "warn", "error"};

QComboBox* level_box(LogLevel level) {
  auto* box = new QComboBox;
  for (const char* name : kLevelNames) box->addItem(QString::fromLatin1(name));
  box->setCurrentIndex(static_cast<int>(level));
  return box;
}

LogLevel level_in(const QComboBox* box) {
  const int i = box != nullptr ? box->currentIndex() : -1;
  return i >= 0 && i < 5 ? kLevels[i] : LogLevel::Info;
}

}  // namespace

LoggingPage::LoggingPage(QWidget* parent)
    : QWidget(parent),
      default_level_(level_box(LogLevel::Info)),
      levels_(new QTableWidget(0, 2)),
      add_(new QPushButton(tr("Add"))),
      remove_(new QPushButton(tr("Remove"))),
      folder_(new QLineEdit),
      max_size_(new QSpinBox),
      max_files_(new QSpinBox),
      echo_(new QCheckBox(tr("Also print to the terminal"))) {
  levels_->setHorizontalHeaderLabels({tr("Logger"), tr("Level")});
  levels_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  levels_->verticalHeader()->hide();
  levels_->setSelectionBehavior(QAbstractItemView::SelectRows);
  levels_->setSelectionMode(QAbstractItemView::SingleSelection);
  levels_->setMinimumHeight(150);
  remove_->setEnabled(false);
  connect(add_, &QPushButton::clicked, this, [this] {
    add_row({}, LogLevel::Debug);
    levels_->setCurrentCell(levels_->rowCount() - 1, 0);
    levels_->editItem(levels_->item(levels_->rowCount() - 1, 0));
  });
  connect(remove_, &QPushButton::clicked, this, [this] {
    if (levels_->currentRow() >= 0) levels_->removeRow(levels_->currentRow());
  });
  connect(levels_, &QTableWidget::itemSelectionChanged, this,
          [this] { remove_->setEnabled(!levels_->selectedItems().isEmpty()); });

  auto* now = new QGroupBox(tr("Levels"));
  auto* now_form = new QFormLayout(now);
  now_form->addRow(tr("Everything else:"), default_level_);
  auto* row_buttons = new QVBoxLayout;
  row_buttons->addWidget(add_);
  row_buttons->addWidget(remove_);
  row_buttons->addStretch(1);
  auto* table_row = new QHBoxLayout;
  table_row->addWidget(levels_, 1);
  table_row->addLayout(row_buttons);
  now_form->addRow(table_row);
  now_form->addRow(preferences_note(
      tr("A logger is named like transport.serial or scheduler; * stands for any part. \"*.wire\" at trace shows "
         "every byte sent and received. Levels change as soon as they are applied.")));

  folder_->setPlaceholderText(tr("None: no log file"));
  folder_->setClearButtonEnabled(true);
  auto* browse = new QPushButton(tr("Browse…"));
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder for log files"), folder_->text());
    if (!dir.isEmpty()) folder_->setText(dir);
  });
  auto* folder_row = new QHBoxLayout;
  folder_row->addWidget(folder_, 1);
  folder_row->addWidget(browse);
  max_size_->setRange(1, 10000);
  max_size_->setSuffix(tr(" MB"));
  max_files_->setRange(1, 1000);

  auto* file = new QGroupBox(tr("Log file"));
  auto* file_form = new QFormLayout(file);
  file_form->addRow(tr("Folder:"), folder_row);
  file_form->addRow(tr("Start a new file at:"), max_size_);
  file_form->addRow(tr("Files kept:"), max_files_);
  file_form->addRow(echo_);
  file_form->addRow(preferences_note(tr("These take effect the next time pychron starts.")));

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(now, 1);  // the table takes what room there is
  layout->addWidget(file);
}

QComboBox* LoggingPage::level_at(int row) const { return qobject_cast<QComboBox*>(levels_->cellWidget(row, 1)); }

void LoggingPage::add_row(const QString& pattern, LogLevel level) {
  const int row = levels_->rowCount();
  levels_->insertRow(row);
  levels_->setItem(row, 0, new QTableWidgetItem(pattern));
  levels_->setCellWidget(row, 1, level_box(level));
}

void LoggingPage::set(const config::LoggingConfig& logging) {
  base_ = logging;
  default_level_->setCurrentIndex(static_cast<int>(logging.default_level));
  levels_->setRowCount(0);
  // By name, so the table reads the same however the file listed them.
  const std::map<std::string, LogLevel> sorted(logging.levels.begin(), logging.levels.end());
  for (const auto& [pattern, level] : sorted) add_row(QString::fromStdString(pattern), level);
  folder_->setText(QString::fromStdString(logging.dir.string()));
  max_size_->setValue(static_cast<int>(logging.max_size_mb));
  max_files_->setValue(static_cast<int>(logging.max_files));
  echo_->setChecked(logging.echo_stderr);
}

config::LoggingConfig LoggingPage::value() const {
  config::LoggingConfig out = base_;
  out.default_level = level_in(default_level_);
  out.levels.clear();
  for (int row = 0; row < levels_->rowCount(); ++row) {
    const QTableWidgetItem* item = levels_->item(row, 0);
    const std::string pattern = item != nullptr ? item->text().trimmed().toStdString() : std::string();
    if (pattern.empty()) continue;
    const LogLevel level = level_in(level_at(row));
    bool replaced = false;
    for (auto& existing : out.levels) {
      if (existing.first == pattern) {
        existing.second = level;
        replaced = true;
      }
    }
    if (!replaced) out.levels.emplace_back(pattern, level);
  }
  out.dir = folder_->text().trimmed().toStdString();
  out.max_size_mb = max_size_->value();
  out.max_files = max_files_->value();
  out.echo_stderr = echo_->isChecked();
  return out;
}

}  // namespace pychron::ui
