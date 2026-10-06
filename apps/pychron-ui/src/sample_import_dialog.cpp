#include "sample_import_dialog.hpp"

#include <filesystem>

#include "pychron/core/user_file.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include "theme.hpp"

namespace pychron::ui {

namespace ps = persistence;

SampleImportDialog::SampleImportDialog(EntryBridge& bridge, QWidget* parent) : QDialog(parent), bridge_(bridge) {
  setWindowTitle(tr("Import Samples"));
  auto* layout = new QVBoxLayout(this);
  auto* sources = new QHBoxLayout();
  auto* open = new QPushButton(tr("Open…"), this);
  auto* template_button = new QPushButton(tr("Write Template…"), this);
  auto* preview_button = new QPushButton(tr("Preview"), this);
  sources->addWidget(open);
  sources->addWidget(template_button);
  sources->addStretch(1);
  sources->addWidget(preview_button);
  layout->addLayout(sources);

  text_ = new QPlainTextEdit(this);
  text_->setPlaceholderText(tr("Open a CSV file, or paste rows copied from a spreadsheet (with a header row)"));
  mapping_ = new QTableWidget(0, 2, this);
  mapping_->setHorizontalHeaderLabels({tr("Column"), tr("Field")});
  mapping_->horizontalHeader()->setStretchLastSection(true);
  auto* top = new QSplitter(Qt::Horizontal, this);
  top->addWidget(text_);
  top->addWidget(mapping_);
  layout->addWidget(top, 1);

  auto* filters = new QHBoxLayout();
  filter_ = new QComboBox(this);
  filter_->addItems({tr("All"), tr("Create"), tr("Exists"), tr("Update"), tr("Error")});
  update_existing_ = new QCheckBox(tr("Update samples that differ"), this);
  auto* export_errors = new QPushButton(tr("Export Errors…"), this);
  filters->addWidget(new QLabel(tr("Show"), this));
  filters->addWidget(filter_);
  filters->addWidget(update_existing_);
  filters->addStretch(1);
  filters->addWidget(export_errors);
  layout->addLayout(filters);

  preview_ = new QTableWidget(0, 7, this);
  preview_->setHorizontalHeaderLabels(
      {tr("Line"), tr("State"), tr("Sample"), tr("Project"), tr("PI"), tr("Material"), tr("Messages")});
  preview_->horizontalHeader()->setStretchLastSection(true);
  preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  layout->addWidget(preview_, 2);
  message_ = new QLabel(this);
  message_->setWordWrap(true);
  layout->addWidget(message_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  import_ = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
  import_->setEnabled(false);
  layout->addWidget(buttons);

  connect(open, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Samples"), {}, tr("CSV (*.csv *.tsv *.txt);;All (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return show_message(tr("Cannot read %1").arg(path), true);
    set_text(QString::fromUtf8(f.readAll()));
    preview();
  });
  connect(template_button, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Template"), QStringLiteral("samples.csv"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return show_message(tr("Cannot write %1").arg(path), true);
    f.write(QByteArray::fromStdString(entry::template_csv()));
    f.close();
    pychron::mark_as_user_file(std::filesystem::path(path.toStdString()));
  });
  connect(export_errors, &QPushButton::clicked, this, [this] {
    if (!plan_ || plan_->errors == 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Errors"), QStringLiteral("errors.csv"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return show_message(tr("Cannot write %1").arg(path), true);
    f.write(QByteArray::fromStdString(entry::errors_csv(*plan_)));
    f.close();
    pychron::mark_as_user_file(std::filesystem::path(path.toStdString()));
  });
  connect(preview_button, &QPushButton::clicked, this, [this] { preview(); });
  connect(filter_, &QComboBox::currentIndexChanged, this, [this] { fill_preview(); });
  connect(update_existing_, &QCheckBox::toggled, this, [this] { preview(); });
  connect(import_, &QPushButton::clicked, this, [this] { import_rows(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  resize(1000, 700);
}

QString SampleImportDialog::message() const { return message_->text(); }

void SampleImportDialog::show_message(const QString& text, bool error) {
  message_->setText(text);
  style::set_tone(message_, error ? style::Tone::Error : style::Tone::Normal);
}

void SampleImportDialog::set_text(const QString& text) {
  text_->setPlainText(text);
  table_.reset();
  plan_.reset();
  mapping_->setRowCount(0);
}

entry::ColumnMapping SampleImportDialog::mapping() const {
  entry::ColumnMapping out;
  for (int r = 0; r < mapping_->rowCount(); ++r) {
    auto* box = qobject_cast<QComboBox*>(mapping_->cellWidget(r, 1));
    const int i = box ? box->currentIndex() : 0;
    out.push_back(i <= 0 ? std::nullopt : std::optional<entry::ImportField>(entry::import_fields()[static_cast<std::size_t>(i - 1)]));
  }
  return out;
}

void SampleImportDialog::fill_mapping() {
  const auto defaults = entry::default_mapping(table_->header);
  mapping_->setRowCount(static_cast<int>(table_->header.size()));
  for (int r = 0; r < mapping_->rowCount(); ++r) {
    mapping_->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(table_->header[static_cast<std::size_t>(r)])));
    auto* box = new QComboBox(mapping_);
    box->addItem(tr("(ignore)"));
    int at = 0;
    for (std::size_t f = 0; f < entry::import_fields().size(); ++f) {
      box->addItem(QString::fromStdString(std::string(entry::field_name(entry::import_fields()[f]))));
      if (defaults[static_cast<std::size_t>(r)] == entry::import_fields()[f]) at = static_cast<int>(f) + 1;
    }
    box->setCurrentIndex(at);
    connect(box, &QComboBox::currentIndexChanged, this, [this] {
      if (!table_) return;
      entry::ImportOptions o{update_existing_->isChecked(), settings_.pi_names_allowed};
      plan_ = entry::plan_sample_import(*table_, mapping(), catalog_, o);
      fill_preview();
    });
    mapping_->setCellWidget(r, 1, box);
  }
}

bool SampleImportDialog::preview() {
  const std::string text = text_->toPlainText().toStdString();
  if (text.empty()) {
    show_message(tr("Nothing to import"), true);
    return false;
  }
  const bool new_table = !table_;
  auto parsed = entry::read_csv(text);
  if (!parsed) {
    show_message(QString::fromStdString(parsed.error().what), true);
    return false;
  }
  table_ = std::move(*parsed);
  struct Loaded {
    entry::CatalogSnapshot catalog;
    entry::EntrySettings settings;
  };
  auto loaded = bridge_.run_sync<Loaded>([](ps::IStore& s, const ps::Actor&) -> Result<Loaded> {
    auto c = entry::read_snapshot(s);
    if (!c) return fail(c.error());
    auto settings = entry::load_settings(s);
    if (!settings) return fail(settings.error());
    return Loaded{std::move(*c), settings->settings};
  });
  if (!loaded) {
    show_message(QString::fromStdString(to_string(loaded.error())), true);
    return false;
  }
  catalog_ = std::move(loaded->catalog);
  settings_ = std::move(loaded->settings);
  if (new_table || mapping_->rowCount() != static_cast<int>(table_->header.size())) fill_mapping();
  entry::ImportOptions o{update_existing_->isChecked(), settings_.pi_names_allowed};
  plan_ = entry::plan_sample_import(*table_, mapping(), catalog_, o);
  fill_preview();
  return true;
}

void SampleImportDialog::fill_preview() {
  preview_->setRowCount(0);
  if (!plan_) return;
  const int filter = filter_->currentIndex();  // 0 all, then Create, Exists, Update, Error
  for (const auto& r : plan_->rows) {
    if (filter > 0 && static_cast<int>(r.state) != filter - 1) continue;
    const int at = preview_->rowCount();
    preview_->insertRow(at);
    QString messages;
    for (const auto& m : r.messages) messages += (messages.isEmpty() ? QString() : QStringLiteral("; ")) + QString::fromStdString(m);
    const QStringList cells{QString::number(r.line), QString::fromStdString(std::string(entry::to_string(r.state))),
                            QString::fromStdString(r.sample), QString::fromStdString(r.project),
                            QString::fromStdString(r.principal_investigator), QString::fromStdString(r.material), messages};
    for (int c = 0; c < cells.size(); ++c) {
      auto* item = new QTableWidgetItem(cells[c]);
      if (r.state == entry::RowState::Error) item->setBackground(theme().error_bg);
      if (r.state == entry::RowState::Create) item->setBackground(theme().diff_added);
      if (r.state == entry::RowState::Update) item->setBackground(theme().diff_changed);
      preview_->setItem(at, c, item);
    }
  }
  const bool writes = plan_->creates > 0 || (plan_->updates > 0 && update_existing_->isChecked());
  import_->setEnabled(plan_->errors == 0 && writes);
  QString summary = tr("%1 to create, %2 differing, %3 stored, %4 in error")
                        .arg(plan_->creates)
                        .arg(plan_->updates)
                        .arg(plan_->exists)
                        .arg(plan_->errors);
  if (!plan_->new_principal_investigators.empty() || !plan_->new_projects.empty() || !plan_->new_materials.empty())
    summary += tr(" — also new: %1 PIs, %2 projects, %3 materials")
                   .arg(plan_->new_principal_investigators.size())
                   .arg(plan_->new_projects.size())
                   .arg(plan_->new_materials.size());
  show_message(summary, plan_->errors > 0);
}

void SampleImportDialog::import_rows() {
  if (!plan_ || plan_->errors > 0) return;
  entry::ImportOptions o{update_existing_->isChecked(), settings_.pi_names_allowed};
  auto batch = entry::to_batch(*plan_, catalog_, o);
  if (batch.edits.empty()) return show_message(tr("Nothing to write"));
  import_->setEnabled(false);
  bridge_.run<ps::CatalogOutcome>(
      this,
      [batch = std::move(batch)](ps::IStore& s, const ps::Actor& a) { return s.apply_catalog_edits(a.client, batch); },
      [this](Result<ps::CatalogOutcome> r) {
        if (!r) {
          show_message(QString::fromStdString(to_string(r.error())), true);
          Q_EMIT imported(false);
          return;
        }
        if (std::holds_alternative<ps::CatalogApplied>(*r)) {
          show_message(tr("Imported"));
          bridge_.notify_changed();
          Q_EMIT imported(true);
          preview();  // everything now reads "exists"
          return;
        }
        show_message(tr("The catalog changed meanwhile; nothing was written. Preview again."), true);
        Q_EMIT imported(false);
      });
}

}  // namespace pychron::ui
