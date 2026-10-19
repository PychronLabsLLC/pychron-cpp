#include "release_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/publish/release.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

namespace pb = publish;

namespace {

std::optional<std::string> text_or_none(const QString& s) {
  const QString t = s.trimmed();
  if (t.isEmpty()) return std::nullopt;
  return t.toStdString();
}

}  // namespace

std::optional<pb::ReleaseSpec> ask_release(QWidget* parent, std::size_t analyses, std::size_t embargoed,
                                           const QStringList& releases) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("New release"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* counts = new QLabel(QObject::tr("%1 analyses, %2 under embargo now").arg(analyses).arg(embargoed), &dialog);
  layout->addWidget(counts);

  auto* form = new QFormLayout;
  auto* name = new QLineEdit(&dialog);
  name->setPlaceholderText(QObject::tr("a short name, as in a file name"));
  auto* title = new QLineEdit(&dialog);
  auto* description = new QPlainTextEdit(&dialog);
  description->setMaximumHeight(80);
  auto* authors = new QTableWidget(1, 3, &dialog);
  authors->setHorizontalHeaderLabels({QObject::tr("Name"), QObject::tr("ORCID"), QObject::tr("Affiliation")});
  authors->horizontalHeader()->setStretchLastSection(true);
  authors->setMaximumHeight(120);
  auto* add_author = new QPushButton(QObject::tr("Add author"), &dialog);
  QObject::connect(add_author, &QPushButton::clicked, authors, [authors] { authors->insertRow(authors->rowCount()); });
  auto* licence = new QComboBox(&dialog);
  licence->setEditable(true);
  licence->addItems({QStringLiteral("CC-BY-4.0"), QStringLiteral("CC-BY-SA-4.0"), QStringLiteral("CC0-1.0")});
  licence->setCurrentText(QString::fromUtf8(pb::kDefaultLicence.data(), static_cast<int>(pb::kDefaultLicence.size())));
  auto* publisher = new QLineEdit(&dialog);
  auto* tags = new QLineEdit(&dialog);
  tags->setPlaceholderText(QObject::tr("kept tags, comma separated; empty: every tag but invalid"));
  auto* embargo_on = new QCheckBox(QObject::tr("Under embargo until"), &dialog);
  auto* embargo = new QDateEdit(QDate::currentDate().addYears(1), &dialog);
  embargo->setCalendarPopup(true);
  embargo->setEnabled(false);
  QObject::connect(embargo_on, &QCheckBox::toggled, embargo, &QWidget::setEnabled);
  auto* embargo_row = new QHBoxLayout;
  embargo_row->addWidget(embargo_on);
  embargo_row->addWidget(embargo, 1);
  auto* supersedes = new QComboBox(&dialog);
  supersedes->addItem(QObject::tr("(none)"));
  supersedes->addItems(releases);

  form->addRow(QObject::tr("Name"), name);
  form->addRow(QObject::tr("Title"), title);
  form->addRow(QObject::tr("Description"), description);
  form->addRow(QObject::tr("Authors"), authors);
  form->addRow(QString(), add_author);
  form->addRow(QObject::tr("Licence"), licence);
  form->addRow(QObject::tr("Publisher"), publisher);
  form->addRow(QObject::tr("Tags"), tags);
  form->addRow(QObject::tr("Embargo"), embargo_row);
  form->addRow(QObject::tr("Supersedes"), supersedes);
  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) return std::nullopt;

  pb::ReleaseSpec spec;
  spec.name = name->text().trimmed().toStdString();
  spec.title = title->text().trimmed().toStdString();
  spec.description = text_or_none(description->toPlainText());
  for (int r = 0; r < authors->rowCount(); ++r) {
    const auto at = [&](int c) { return authors->item(r, c) ? authors->item(r, c)->text() : QString(); };
    const auto who = text_or_none(at(0));
    if (!who) continue;
    spec.authors.push_back({*who, text_or_none(at(1)), text_or_none(at(2))});
  }
  spec.licence = licence->currentText().trimmed().toStdString();
  spec.publisher = text_or_none(publisher->text());
  for (const QString& t : tags->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
    if (!t.trimmed().isEmpty()) spec.tags.push_back(t.trimmed().toStdString());
  if (embargo_on->isChecked()) {
    const QDate d = embargo->date();
    if (auto until = persistence::UtcTime::parse(d.toString(Qt::ISODate).toStdString() + "T00:00:00Z")) spec.embargo_until = *until;
  }
  if (supersedes->currentIndex() > 0) spec.supersedes = supersedes->currentText().toStdString();
  return spec;
}

}  // namespace pychron::ui
