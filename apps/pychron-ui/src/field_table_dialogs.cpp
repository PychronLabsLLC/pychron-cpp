#include "field_table_dialogs.hpp"

#include <cmath>
#include <optional>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pychron::ui {

namespace sp = pychron::spectrometer;

namespace {

QString number(double v) { return QString::number(v, 'f', 5); }
QString maybe(const std::optional<double>& v) { return v ? number(*v) : QStringLiteral("—"); }
QString q(const std::string& s) { return QString::fromStdString(s); }

void add_row(QTableWidget* t, const QStringList& cells) {
  const int r = t->rowCount();
  t->insertRow(r);
  for (int c = 0; c < cells.size(); ++c) {
    auto* item = new QTableWidgetItem(cells[c]);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (c >= 2) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    t->setItem(r, c, item);
  }
}

QDialogButtonBox* ok_cancel(QDialog* d) {
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, d, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
  return buttons;
}

}  // namespace

FieldTableCompareDialog::FieldTableCompareDialog(const sp::TableDiff& diff, const QString& old_version,
                                                 const QString& new_version, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(tr("Compare %1 with %2").arg(old_version, new_version));
  resize(620, 480);
  auto* col = new QVBoxLayout(this);
  col->addWidget(new QLabel(tr("Old: %1    New: %2").arg(old_version, new_version)));

  auto* fits = new QListWidget;
  fits->setObjectName(QStringLiteral("fit_changes"));
  for (const auto& line : diff.fit_changes) fits->addItem(q(line));
  fits->setMaximumHeight(90);
  fits->setVisible(!diff.fit_changes.empty());
  col->addWidget(fits);

  auto* table = new QTableWidget(0, 5);
  table->setObjectName(QStringLiteral("differences"));
  table->setHorizontalHeaderLabels({tr("Isotope"), tr("Detector"), tr("Old"), tr("New"), QStringLiteral("Δ")});
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  for (const auto& r : diff.rows) {
    add_row(table, {q(r.isotope), r.change == sp::RowChange::Added ? tr("row added") : tr("row removed"), {}, {}, {}});
  }
  for (const auto& m : diff.masses) {
    add_row(table, {q(m.isotope), tr("mass"), number(m.old_mass), number(m.new_mass), number(m.new_mass - m.old_mass)});
  }
  for (const auto& c : diff.cells) {
    const QString delta = c.old_value && c.new_value ? number(*c.new_value - *c.old_value) : QString();
    add_row(table, {q(c.isotope), q(c.detector), maybe(c.old_value), maybe(c.new_value), delta});
  }
  rows_ = table->rowCount();
  col->addWidget(table, 1);
  if (diff.empty()) col->addWidget(new QLabel(tr("No differences.")));

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  col->addWidget(buttons);
}

QDialog* make_text_compare_dialog(const QString& old_title, const QString& old_text, const QString& new_title,
                                  const QString& new_text, QWidget* parent) {
  auto* d = new QDialog(parent);  // the caller (or WA_DeleteOnClose) owns it
  d->setWindowTitle(QObject::tr("Compare %1 with %2").arg(old_title, new_title));
  d->resize(820, 520);
  auto* col = new QVBoxLayout(d);
  auto* row = new QHBoxLayout;
  for (const auto& [title, text] : {std::pair{old_title, old_text}, std::pair{new_title, new_text}}) {
    auto* side = new QVBoxLayout;
    side->addWidget(new QLabel(title));
    auto* pane = new QPlainTextEdit(text);
    pane->setReadOnly(true);
    pane->setLineWrapMode(QPlainTextEdit::NoWrap);
    side->addWidget(pane, 1);
    row->addLayout(side, 1);
  }
  col->addLayout(row, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  QObject::connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
  col->addWidget(buttons);
  return d;
}

FieldTableSetValueDialog::FieldTableSetValueDialog(const std::vector<std::string>& detectors,
                                                   const std::vector<std::string>& isotopes, bool propagate,
                                                   QWidget* parent)
    : QDialog(parent),
      detector_(new QComboBox),
      isotope_(new QComboBox),
      value_(new QLineEdit),
      propagate_(new QCheckBox(tr("Propagate the offset to the other detectors"))) {
  setWindowTitle(tr("Set Value"));
  detector_->setObjectName(QStringLiteral("detector"));
  isotope_->setObjectName(QStringLiteral("isotope"));
  value_->setObjectName(QStringLiteral("value"));
  propagate_->setObjectName(QStringLiteral("propagate"));
  for (const auto& d : detectors) detector_->addItem(q(d));
  isotope_->setEditable(true);
  for (const auto& i : isotopes) isotope_->addItem(q(i));
  propagate_->setChecked(propagate);
  auto* form = new QFormLayout(this);
  form->addRow(tr("Detector"), detector_);
  form->addRow(tr("Isotope"), isotope_);
  form->addRow(tr("Value"), value_);
  form->addRow(QString(), propagate_);
  form->addRow(ok_cancel(this));
}

std::string FieldTableSetValueDialog::detector() const { return detector_->currentText().toStdString(); }

std::string FieldTableSetValueDialog::isotope() const { return isotope_->currentText().trimmed().toStdString(); }

std::optional<double> FieldTableSetValueDialog::value() const {
  bool ok = false;
  const double v = value_->text().trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(v)) return std::nullopt;
  return v;
}

bool FieldTableSetValueDialog::propagate() const { return propagate_->isChecked(); }

void FieldTableSetValueDialog::set_detector(const std::string& detector) {
  const int i = detector_->findText(q(detector));
  if (i >= 0) detector_->setCurrentIndex(i);
}

void FieldTableSetValueDialog::set_isotope(const std::string& isotope) { isotope_->setCurrentText(q(isotope)); }

void FieldTableSetValueDialog::set_value(double value) { value_->setText(QString::number(value, 'g', 17)); }

void FieldTableSetValueDialog::set_propagate(bool on) { propagate_->setChecked(on); }

FieldTableAddRowDialog::FieldTableAddRowDialog(const std::vector<std::string>& names, QWidget* parent)
    : QDialog(parent), isotope_(new QComboBox) {
  setWindowTitle(tr("Add Row"));
  isotope_->setObjectName(QStringLiteral("isotope"));
  isotope_->setEditable(true);
  for (const auto& n : names) isotope_->addItem(q(n));
  isotope_->setCurrentText(QString());
  auto* form = new QFormLayout(this);
  form->addRow(tr("Isotope"), isotope_);
  form->addRow(new QLabel(tr("The mass comes from the molecular weights; values start empty.")));
  form->addRow(ok_cancel(this));
}

QString FieldTableAddRowDialog::isotope() const { return isotope_->currentText().trimmed(); }

void FieldTableAddRowDialog::set_isotope(const QString& isotope) { isotope_->setCurrentText(isotope); }

}  // namespace pychron::ui
