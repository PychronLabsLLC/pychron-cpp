#include "package_dialogs.hpp"

#include <sstream>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>

#include "holder_view.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace ps = persistence;

namespace {

QDialogButtonBox* ok_cancel(QDialog* dialog, const QString& ok_text = {}) {
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  if (!ok_text.isEmpty()) buttons->button(QDialogButtonBox::Ok)->setText(ok_text);
  QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  return buttons;
}

void say(QLabel* label, const QString& text, bool error) {
  label->setText(text);
  style::set_tone(label, error ? style::Tone::Error : style::Tone::Normal);
}

std::optional<double> number(const QLineEdit* edit, bool* bad) {
  const QString t = edit->text().trimmed();
  if (t.isEmpty()) return std::nullopt;
  bool ok = false;
  const double v = t.toDouble(&ok);
  if (!ok) *bad = true;
  return ok ? std::optional<double>(v) : std::nullopt;
}

std::optional<ps::UtcTime> utc_of(const QString& local) {
  QDateTime dt = QDateTime::fromString(local.trimmed(), QStringLiteral("yyyy-MM-dd HH:mm"));
  if (!dt.isValid()) return std::nullopt;
  dt.setTimeZone(QTimeZone::systemTimeZone());
  return ps::UtcTime::parse(dt.toUTC().toString(Qt::ISODate).toStdString());
}

QString id_text(const ps::Uuid& u) { return QString::fromStdString(u.str()); }

std::optional<ps::Uuid> id_of(const QComboBox* box) {
  const QString t = box->currentData().toString();
  if (t.isEmpty()) return std::nullopt;
  return ps::Uuid::parse(t.toStdString());
}

}  // namespace

// ---------------------------------------------------------------- ClearFieldsDialog

ClearFieldsDialog::ClearFieldsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Clear Fields"));
  auto* layout = new QVBoxLayout(this);
  sample_ = new QCheckBox(tr("Sample (an identifier stays)"), this);
  weight_ = new QCheckBox(tr("Weight"), this);
  packet_ = new QCheckBox(tr("Packet"), this);
  note_ = new QCheckBox(tr("Note"), this);
  for (QCheckBox* box : {sample_, weight_, packet_, note_}) {
    box->setChecked(true);
    layout->addWidget(box);
  }
  layout->addWidget(ok_cancel(this, tr("Clear")));
}

std::set<entry::SheetField> ClearFieldsDialog::fields() const {
  std::set<entry::SheetField> out;
  if (sample_->isChecked()) out.insert(entry::SheetField::Sample);
  if (weight_->isChecked()) out.insert(entry::SheetField::Weight);
  if (packet_->isChecked()) out.insert(entry::SheetField::Packet);
  if (note_->isChecked()) out.insert(entry::SheetField::Note);
  return out;
}

// ---------------------------------------------------------------- NewPackageDialog

NewPackageDialog::NewPackageDialog(EntryBridge& bridge, QWidget* parent) : QDialog(parent), bridge_(bridge) {
  setWindowTitle(tr("New Package"));
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();
  kind_ = new QComboBox(this);
  kind_->addItem(tr("irradiation"), QStringLiteral("irradiation"));
  kind_->addItem(tr("package"), QStringLiteral("package"));
  name_ = new QLineEdit(this);
  levels_ = new QLineEdit(QStringLiteral("A"), this);
  levels_->setToolTip(tr("A-C, or A,B,D"));
  holder_ = new QComboBox(this);
  z_ = new QLineEdit(this);
  form->addRow(tr("Kind"), kind_);
  form->addRow(tr("Name"), name_);
  form->addRow(tr("Levels"), levels_);
  form->addRow(tr("Holder"), holder_);
  form->addRow(tr("z"), z_);
  layout->addLayout(form);

  irradiation_part_ = new QWidget(this);
  auto* il = new QVBoxLayout(irradiation_part_);
  il->setContentsMargins(0, 0, 0, 0);
  auto* rform = new QFormLayout();
  reactor_ = new QComboBox(irradiation_part_);
  reactor_->setEditable(true);
  rform->addRow(tr("Reactor"), reactor_);
  il->addLayout(rform);
  doses_ = new QTableWidget(0, 3, irradiation_part_);
  doses_->setHorizontalHeaderLabels({tr("Power"), tr("Start (local)"), tr("End (local)")});
  doses_->horizontalHeader()->setStretchLastSection(true);
  auto* add = new QPushButton(tr("Add Dose"), irradiation_part_);
  il->addWidget(doses_);
  il->addWidget(add);
  layout->addWidget(irradiation_part_);
  message_ = new QLabel(this);
  message_->setWordWrap(true);
  layout->addWidget(message_);
  layout->addWidget(ok_cancel(this, tr("Create")));

  connect(add, &QPushButton::clicked, this, [this] {
    const int at = doses_->rowCount();
    QDate day = QDate::currentDate();
    if (at > 0) day = QDateTime::fromString(doses_->item(at - 1, 2)->text(), QStringLiteral("yyyy-MM-dd HH:mm")).date();
    doses_->insertRow(at);
    doses_->setItem(at, 0, new QTableWidgetItem(QStringLiteral("1.0")));
    doses_->setItem(at, 1, new QTableWidgetItem(day.toString(Qt::ISODate) + QStringLiteral(" 08:00")));
    doses_->setItem(at, 2, new QTableWidgetItem(day.toString(Qt::ISODate) + QStringLiteral(" 17:00")));
  });
  connect(kind_, &QComboBox::currentIndexChanged, this, [this] { update_kind(); });

  struct Defaults {
    entry::EntrySettings settings;
    std::vector<std::string> names;
    std::vector<ps::RefObjectRow> holders;
    std::map<std::string, ps::ProductionValue> reactors;
  };
  auto d = bridge_.run_sync<Defaults>([](ps::IStore& s, const ps::Actor&) -> Result<Defaults> {
    Defaults out;
    auto settings = entry::load_settings(s);
    if (!settings) return fail(settings.error());
    out.settings = settings->settings;
    auto packages = s.irradiations();
    if (!packages) return fail(packages.error());
    for (const auto& p : *packages) out.names.push_back(p.name);
    auto holders = s.ref_objects(ps::RefType::IrradiationHolder, std::nullopt);
    if (!holders) return fail(holders.error());
    out.holders = std::move(*holders);
    auto reactors = entry::load_reactors(s);
    if (!reactors) return fail(reactors.error());
    out.reactors = std::move(*reactors);
    return out;
  });
  if (!d) {
    say(message_, QString::fromStdString(to_string(d.error())), true);
    return;
  }
  name_->setText(QString::fromStdString(entry::next_package_name(d->names, d->settings.package_prefix)));
  kind_->setCurrentIndex(d->settings.default_package_kind == "package" ? 1 : 0);
  holders_ = std::move(d->holders);
  reactors_ = std::move(d->reactors);
  holder_->addItem(tr("(none)"), QString());
  for (const auto& h : holders_) holder_->addItem(QString::fromStdString(h.key), id_text(h.uuid));
  for (const auto& [name, value] : reactors_) reactor_->addItem(QString::fromStdString(name));
  update_kind();
}

QString NewPackageDialog::message() const { return message_->text(); }

void NewPackageDialog::update_kind() { irradiation_part_->setVisible(kind_->currentData().toString() == "irradiation"); }

void NewPackageDialog::accept() {
  entry::NewPackage p;
  p.name = entry::trim(name_->text().toStdString());
  p.kind = kind_->currentData().toString().toStdString();
  bool bad = false;
  const auto z = number(z_, &bad);
  if (bad) return say(message_, tr("z is a number"), true);
  const std::string spec = entry::trim(levels_->text().toStdString());
  std::vector<std::string> names;
  if (spec.size() == 3 && spec[1] == '-' && spec[0] >= 'A' && spec[2] <= 'Z' && spec[0] <= spec[2]) {
    for (char c = spec[0]; c <= spec[2]; ++c) names.emplace_back(1, c);
  } else {
    std::stringstream s(spec);
    std::string part;
    while (std::getline(s, part, ','))
      if (!entry::trim(part).empty()) names.push_back(entry::trim(part));
  }
  for (const auto& n : names) p.levels.push_back(entry::NewLevel{n, id_of(holder_), z, std::nullopt});
  if (p.kind == "irradiation") {
    const std::string reactor = entry::trim(reactor_->currentText().toStdString());
    if (!reactor.empty()) {
      p.reactor = reactor;
      if (auto it = reactors_.find(reactor); it != reactors_.end()) p.production = it->second;
    }
    for (int r = 0; r < doses_->rowCount(); ++r) {
      bool ok = false;
      const double power = doses_->item(r, 0) ? doses_->item(r, 0)->text().toDouble(&ok) : 0;
      const auto start = doses_->item(r, 1) ? utc_of(doses_->item(r, 1)->text()) : std::nullopt;
      const auto end = doses_->item(r, 2) ? utc_of(doses_->item(r, 2)->text()) : std::nullopt;
      if (!ok || !start || !end) return say(message_, tr("Dose %1: a power, then yyyy-MM-dd HH:mm times").arg(r + 1), true);
      p.doses.push_back(ps::Dose{r, power, *start, *end});
    }
  }
  auto made = bridge_.run_sync<entry::CreatedPackage>(
      [p](ps::IStore& s, const ps::Actor& a) { return entry::create_package(s, a, p); });
  if (!made) return say(message_, QString::fromStdString(made.error().what), true);
  if (!made->levels.empty()) first_level_ = made->levels.front();
  QDialog::accept();
}

// ---------------------------------------------------------------- NewLevelDialog

NewLevelDialog::NewLevelDialog(EntryBridge& bridge, ps::IrradiationRow package, const entry::NewLevel& defaults,
                               std::optional<ps::Uuid> production, const std::vector<ps::RefObjectRow>& holders,
                               const std::vector<entry::NamedProduction>& productions, QWidget* parent)
    : QDialog(parent), bridge_(bridge), package_(std::move(package)) {
  setWindowTitle(tr("New Level of %1").arg(QString::fromStdString(package_.name)));
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();
  name_ = new QLineEdit(QString::fromStdString(defaults.name), this);
  holder_ = new QComboBox(this);
  holder_->addItem(tr("(none)"), QString());
  for (const auto& h : holders) holder_->addItem(QString::fromStdString(h.key), id_text(h.uuid));
  if (defaults.holder) holder_->setCurrentIndex(std::max(0, holder_->findData(id_text(*defaults.holder))));
  z_ = new QLineEdit(defaults.z ? QString::number(*defaults.z, 'g', 10) : QString(), this);
  production_ = new QComboBox(this);
  production_->addItem(tr("(none)"), QString());
  for (const auto& p : productions) production_->addItem(QString::fromStdString(p.name), id_text(p.ref_object));
  if (production) production_->setCurrentIndex(std::max(0, production_->findData(id_text(*production))));
  production_->setEnabled(package_.kind == "irradiation");
  note_ = new QLineEdit(this);
  form->addRow(tr("Name"), name_);
  form->addRow(tr("Holder"), holder_);
  form->addRow(tr("z"), z_);
  form->addRow(tr("Production"), production_);
  form->addRow(tr("Note"), note_);
  layout->addLayout(form);
  message_ = new QLabel(this);
  layout->addWidget(message_);
  layout->addWidget(ok_cancel(this, tr("Create")));
}

void NewLevelDialog::accept() {
  bool bad = false;
  entry::NewLevel l;
  l.name = entry::trim(name_->text().toStdString());
  l.holder = id_of(holder_);
  l.z = number(z_, &bad);
  if (bad) return say(message_, tr("z is a number"), true);
  if (const QString n = note_->text().trimmed(); !n.isEmpty()) l.note = n.toStdString();
  const auto production = package_.kind == "irradiation" ? id_of(production_) : std::nullopt;
  auto made = bridge_.run_sync<ps::Uuid>([pkg = package_, l, production](ps::IStore& s, const ps::Actor& a) {
    return entry::add_level(s, a, pkg, l, production);
  });
  if (!made) return say(message_, QString::fromStdString(made.error().what), true);
  level_ = *made;
  QDialog::accept();
}

// ---------------------------------------------------------------- ProductionDialog

ProductionDialog::ProductionDialog(EntryBridge& bridge, ps::IrradiationRow package,
                                   std::vector<entry::NamedProduction> productions, const QString& current,
                                   QWidget* parent)
    : QDialog(parent), bridge_(bridge), package_(std::move(package)), productions_(std::move(productions)) {
  setWindowTitle(tr("Productions of %1").arg(QString::fromStdString(package_.name)));
  auto* layout = new QVBoxLayout(this);
  auto* top = new QHBoxLayout();
  which_ = new QComboBox(this);
  for (const auto& p : productions_) which_->addItem(QString::fromStdString(p.name), id_text(p.ref_object));
  which_->addItem(tr("New…"), QString());
  new_name_ = new QLineEdit(this);
  new_name_->setPlaceholderText(tr("new production name"));
  reactor_ = new QComboBox(this);
  auto* copy = new QPushButton(tr("Copy Reactor Default"), this);
  top->addWidget(which_);
  top->addWidget(new_name_);
  top->addStretch(1);
  top->addWidget(reactor_);
  top->addWidget(copy);
  layout->addLayout(top);
  ratios_ = new QTableWidget(static_cast<int>(entry::production_keys().size()), 3, this);
  ratios_->setHorizontalHeaderLabels({tr("Ratio"), tr("Value"), tr("Error")});
  for (std::size_t i = 0; i < entry::production_keys().size(); ++i) {
    auto* key = new QTableWidgetItem(QString::fromStdString(entry::production_keys()[i]));
    key->setFlags(key->flags() & ~Qt::ItemIsEditable);
    ratios_->setItem(static_cast<int>(i), 0, key);
  }
  layout->addWidget(ratios_);
  message_ = new QLabel(this);
  layout->addWidget(message_);
  layout->addWidget(ok_cancel(this, tr("Save")));

  auto reactors = bridge_.run_sync<std::map<std::string, ps::ProductionValue>>(
      [](ps::IStore& s, const ps::Actor&) { return entry::load_reactors(s); });
  if (reactors) reactors_ = std::move(*reactors);
  for (const auto& [name, value] : reactors_) reactor_->addItem(QString::fromStdString(name));
  connect(which_, &QComboBox::currentIndexChanged, this, [this](int i) { show_production(i); });
  connect(copy, &QPushButton::clicked, this, [this] {
    auto it = reactors_.find(reactor_->currentText().toStdString());
    if (it == reactors_.end()) return;
    for (int r = 0; r < ratios_->rowCount(); ++r) {
      ratios_->setItem(r, 1, new QTableWidgetItem());
      ratios_->setItem(r, 2, new QTableWidgetItem());
      for (const auto& ratio : it->second.ratios)
        if (ratio.key == entry::production_keys()[static_cast<std::size_t>(r)]) {
          ratios_->setItem(r, 1, new QTableWidgetItem(QString::number(ratio.value, 'g', 10)));
          ratios_->setItem(r, 2, new QTableWidgetItem(QString::number(ratio.error, 'g', 10)));
        }
    }
  });
  const int at = which_->findData(current);
  which_->setCurrentIndex(at < 0 ? 0 : at);
  show_production(which_->currentIndex());
}

void ProductionDialog::show_production(int index) {
  new_name_->setVisible(which_->itemData(index).toString().isEmpty());
  const ps::ProductionValue* value = index >= 0 && index < static_cast<int>(productions_.size())
                                         ? &productions_[static_cast<std::size_t>(index)].value
                                         : nullptr;
  for (int r = 0; r < ratios_->rowCount(); ++r) {
    ratios_->setItem(r, 1, new QTableWidgetItem());
    ratios_->setItem(r, 2, new QTableWidgetItem());
    if (!value) continue;
    for (const auto& ratio : value->ratios)
      if (ratio.key == entry::production_keys()[static_cast<std::size_t>(r)]) {
        ratios_->setItem(r, 1, new QTableWidgetItem(QString::number(ratio.value, 'g', 10)));
        ratios_->setItem(r, 2, new QTableWidgetItem(QString::number(ratio.error, 'g', 10)));
      }
  }
}

void ProductionDialog::accept() {
  const int index = which_->currentIndex();
  const bool fresh = which_->itemData(index).toString().isEmpty();
  ps::ProductionValue value;
  std::optional<ps::Uuid> object, head;
  std::string name;
  if (fresh) {
    name = entry::trim(new_name_->text().toStdString());
  } else {
    const auto& p = productions_[static_cast<std::size_t>(index)];
    name = p.name;
    object = p.ref_object;
    head = p.head;
    value = p.value;
    value.ratios.clear();
  }
  for (int r = 0; r < ratios_->rowCount(); ++r) {
    const QString v = ratios_->item(r, 1) ? ratios_->item(r, 1)->text().trimmed() : QString();
    const QString e = ratios_->item(r, 2) ? ratios_->item(r, 2)->text().trimmed() : QString();
    if (v.isEmpty() && e.isEmpty()) continue;
    bool ok1 = false, ok2 = e.isEmpty();
    const double value_number = v.toDouble(&ok1);
    const double error_number = e.isEmpty() ? 0 : e.toDouble(&ok2);
    if (!ok1 || !ok2) return say(message_, tr("%1: value and error are numbers").arg(ratios_->item(r, 0)->text()), true);
    value.ratios.push_back(ps::ProductionRatio{entry::production_keys()[static_cast<std::size_t>(r)], value_number, error_number});
  }
  auto saved = bridge_.run_sync<ps::Uuid>([pkg = package_, name, value, object, head](ps::IStore& s, const ps::Actor& a) {
    return entry::save_production(s, a, pkg, name, value, object, head);
  });
  if (!saved) return say(message_, QString::fromStdString(saved.error().what), true);
  QDialog::accept();
}

// ---------------------------------------------------------------- IdentifierDialog

IdentifierDialog::IdentifierDialog(EntryBridge& bridge, ps::IrradiationRow package, QWidget* parent)
    : QDialog(parent), bridge_(bridge), package_(std::move(package)) {
  setWindowTitle(tr("Generate Identifiers — %1").arg(QString::fromStdString(package_.name)));
  auto* layout = new QVBoxLayout(this);
  overwrite_ = new QCheckBox(tr("Renumber identifiers nothing has used yet"), this);
  warnings_ = new QListWidget(this);
  warnings_->setMaximumHeight(90);
  preview_ = new QTableWidget(0, 4, this);
  preview_->setHorizontalHeaderLabels({tr("Level"), tr("Hole"), tr("Sample"), tr("Identifier")});
  preview_->horizontalHeader()->setStretchLastSection(true);
  preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  message_ = new QLabel(this);
  message_->setWordWrap(true);
  layout->addWidget(overwrite_);
  layout->addWidget(warnings_);
  layout->addWidget(preview_, 1);
  layout->addWidget(message_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  auto* commit_button = buttons->addButton(tr("Write Identifiers"), QDialogButtonBox::ActionRole);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(commit_button, &QPushButton::clicked, this, [this] { commit(); });
  connect(overwrite_, &QCheckBox::toggled, this, [this] { replan(); });
  resize(640, 600);
  replan();
}

QString IdentifierDialog::message() const { return message_->text(); }

bool IdentifierDialog::replan() {
  struct Planned {
    entry::IdentifierPlan plan;
    std::vector<std::string> warnings;
  };
  auto r = bridge_.run_sync<Planned>([pkg = package_, overwrite = overwrite_->isChecked()](
                                         ps::IStore& s, const ps::Actor&) -> Result<Planned> {
    auto sheets = entry::package_sheets(s, pkg.uuid);
    if (!sheets) return fail(sheets.error());
    auto last = entry::current_last(s);
    if (!last) return fail(last.error());
    auto settings = entry::load_settings(s);
    if (!settings) return fail(settings.error());
    Planned out;
    out.plan = entry::plan_identifiers(*sheets, *last, overwrite);
    if (pkg.kind == "irradiation") out.warnings = entry::human_error_checks(*sheets, settings->settings, pkg.name);
    return out;
  });
  if (!r) {
    say(message_, QString::fromStdString(to_string(r.error())), true);
    return false;
  }
  plan_ = std::move(r->plan);
  warnings_->clear();
  for (const auto& w : r->warnings) warnings_->addItem(QString::fromStdString(w));
  warnings_->setVisible(!r->warnings.empty());
  preview_->setRowCount(0);
  for (const auto& a : plan_.assignments) {
    const int at = preview_->rowCount();
    preview_->insertRow(at);
    preview_->setItem(at, 0, new QTableWidgetItem(QString::fromStdString(a.level)));
    preview_->setItem(at, 1, new QTableWidgetItem(QString::number(a.position_number)));
    preview_->setItem(at, 2, new QTableWidgetItem(QString::fromStdString(a.sample)));
    preview_->setItem(at, 3, new QTableWidgetItem(a.current ? QStringLiteral("%1 → %2").arg(QString::fromStdString(*a.current)).arg(a.number)
                                                            : QString::number(a.number)));
  }
  say(message_,
      plan_.assignments.empty() ? tr("Every position with a sample has an identifier")
                                : tr("%1 identifiers, %2 to %3").arg(plan_.assignments.size()).arg(plan_.expected_last + 1).arg(plan_.last),
      false);
  return true;
}

void IdentifierDialog::commit() {
  if (plan_.assignments.empty()) return;
  bridge_.run<ps::AllocationOutcome>(
      this,
      [allocation = plan_.allocation()](ps::IStore& s, const ps::Actor& a) { return s.allocate_identifiers(a.client, allocation); },
      [this](Result<ps::AllocationOutcome> r) {
        if (!r) return say(message_, QString::fromStdString(to_string(r.error())), true);
        if (std::holds_alternative<ps::CatalogApplied>(*r)) {
          Q_EMIT allocated();
          QDialog::accept();
          return;
        }
        if (std::holds_alternative<ps::AllocationStale>(*r)) {
          replan();
          return say(message_, tr("Identifiers were given out meanwhile; this is the new plan. Write again to use it."),
                     true);
        }
        QStringList lines;
        for (const auto& x : std::get<std::vector<ps::Refusal>>(*r)) lines << QString::fromStdString(x.what);
        say(message_, tr("Nothing was written: %1").arg(lines.join(QStringLiteral("; "))), true);
      });
}

// ---------------------------------------------------------------- HoldersDialog

HoldersDialog::HoldersDialog(EntryBridge& bridge, QWidget* parent) : QDialog(parent), bridge_(bridge) {
  setWindowTitle(tr("Irradiation Holders"));
  auto* layout = new QHBoxLayout(this);
  auto* left = new QVBoxLayout();
  list_ = new QListWidget(this);
  auto* import_button = new QPushButton(tr("Import…"), this);
  message_ = new QLabel(this);
  message_->setWordWrap(true);
  left->addWidget(list_, 1);
  left->addWidget(import_button);
  left->addWidget(message_);
  view_ = new HolderView(this);
  layout->addLayout(left);
  layout->addWidget(view_, 1);
  connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row < 0 || row >= static_cast<int>(holders_.size())) return view_->set_holder(std::nullopt);
    auto it = values_.find(holders_[static_cast<std::size_t>(row)].uuid);
    view_->set_holder(it == values_.end() ? std::nullopt : std::optional<ps::HolderValue>(it->second));
  });
  connect(import_button, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Holder"), {}, tr("Holder (*.txt);;All (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return say(message_, tr("Cannot read %1").arg(path), true);
    if (auto r = import_text(QFileInfo(path).completeBaseName(), QString::fromUtf8(f.readAll())); !r)
      say(message_, QString::fromStdString(r.error().what), true);
  });
  resize(700, 450);
  reload();
}

void HoldersDialog::reload() {
  using Loaded = std::pair<std::vector<ps::RefObjectRow>, std::map<ps::Uuid, ps::HolderValue>>;
  auto r = bridge_.run_sync<Loaded>([](ps::IStore& s, const ps::Actor&) -> Result<Loaded> {
    auto rows = s.ref_objects(ps::RefType::IrradiationHolder, std::nullopt);
    if (!rows) return fail(rows.error());
    std::map<ps::Uuid, ps::HolderValue> values;
    for (const auto& h : *rows) {
      auto v = entry::load_holder(s, h.uuid);
      if (!v) return fail(v.error());
      if (*v) values.emplace(h.uuid, **v);
    }
    return Loaded{std::move(*rows), std::move(values)};
  });
  if (!r) return say(message_, QString::fromStdString(to_string(r.error())), true);
  holders_ = std::move(r->first);
  values_ = std::move(r->second);
  list_->clear();
  for (const auto& h : holders_) {
    auto it = values_.find(h.uuid);
    list_->addItem(QStringLiteral("%1 (%2 holes)").arg(QString::fromStdString(h.key)).arg(it == values_.end() ? 0 : it->second.holes.size()));
  }
}

Result<void> HoldersDialog::import_text(const QString& name, const QString& text) {
  auto holder = entry::read_holder(text.toStdString());
  if (!holder) return fail(holder.error());
  auto saved = bridge_.run_sync<ps::Uuid>([name = name.toStdString(), h = *holder](ps::IStore& s, const ps::Actor& a) {
    return entry::save_holder(s, a, name, h);
  });
  if (!saved) return fail(saved.error());
  reload();
  say(message_, tr("Saved %1").arg(name), false);
  bridge_.notify_changed();
  return {};
}

// ---------------------------------------------------------------- EntrySettingsDialog

EntrySettingsDialog::EntrySettingsDialog(EntryBridge& bridge, QWidget* parent) : QDialog(parent), bridge_(bridge) {
  setWindowTitle(tr("Entry Settings"));
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();
  prefix_ = new QLineEdit(this);
  default_kind_ = new QComboBox(this);
  default_kind_->addItems({QStringLiteral("irradiation"), QStringLiteral("package")});
  pi_names_ = new QLineEdit(this);
  pi_names_->setToolTip(tr("Names accepted as a PI that are not \"Last, F\", separated by ;"));
  monitor_sample_ = new QLineEdit(this);
  monitor_material_ = new QLineEdit(this);
  project_prefix_ = new QLineEdit(this);
  create_project_ = new QCheckBox(tr("Create the irradiation's monitor project"), this);
  j_multiplier_ = new QLineEdit(this);
  null_rows_ = new QComboBox(this);
  null_rows_->addItems({QStringLiteral("allow"), QStringLiteral("packet")});
  form->addRow(tr("Package name prefix"), prefix_);
  form->addRow(tr("New packages are"), default_kind_);
  form->addRow(tr("Lab PI names"), pi_names_);
  form->addRow(tr("Monitor sample"), monitor_sample_);
  form->addRow(tr("Monitor material"), monitor_material_);
  form->addRow(tr("Irradiation project prefix"), project_prefix_);
  form->addRow(QString(), create_project_);
  form->addRow(tr("Estimated J per hour"), j_multiplier_);
  form->addRow(tr("Positions without an identifier"), null_rows_);
  layout->addLayout(form);
  message_ = new QLabel(this);
  layout->addWidget(message_);
  layout->addWidget(ok_cancel(this, tr("Save")));

  auto loaded = bridge_.run_sync<entry::LoadedSettings>([](ps::IStore& s, const ps::Actor&) { return entry::load_settings(s); });
  if (!loaded) {
    say(message_, QString::fromStdString(to_string(loaded.error())), true);
    return;
  }
  loaded_ = std::move(*loaded);
  const auto& st = loaded_.settings;
  prefix_->setText(QString::fromStdString(st.package_prefix));
  default_kind_->setCurrentText(QString::fromStdString(st.default_package_kind));
  QStringList names;
  for (const auto& n : st.pi_names_allowed) names << QString::fromStdString(n);
  pi_names_->setText(names.join(QStringLiteral("; ")));
  monitor_sample_->setText(QString::fromStdString(st.monitor_sample));
  monitor_material_->setText(QString::fromStdString(st.monitor_material));
  project_prefix_->setText(QString::fromStdString(st.irradiation_project_prefix));
  create_project_->setChecked(st.create_irradiation_project);
  j_multiplier_->setText(QString::number(st.j_multiplier, 'g', 10));
  null_rows_->setCurrentText(QString::fromStdString(st.null_identifier_rows));
}

void EntrySettingsDialog::accept() {
  entry::EntrySettings st = loaded_.settings;
  st.package_prefix = prefix_->text().trimmed().toStdString();
  st.default_package_kind = default_kind_->currentText().toStdString();
  st.pi_names_allowed.clear();
  for (const QString& n : pi_names_->text().split(QLatin1Char(';'), Qt::SkipEmptyParts))
    if (!n.trimmed().isEmpty()) st.pi_names_allowed.push_back(n.trimmed().toStdString());
  st.monitor_sample = monitor_sample_->text().trimmed().toStdString();
  st.monitor_material = monitor_material_->text().trimmed().toStdString();
  st.irradiation_project_prefix = project_prefix_->text().trimmed().toStdString();
  st.create_irradiation_project = create_project_->isChecked();
  bool ok = false;
  st.j_multiplier = j_multiplier_->text().toDouble(&ok);
  if (!ok) return say(message_, tr("Estimated J per hour is a number"), true);
  st.null_identifier_rows = null_rows_->currentText().toStdString();
  auto saved = bridge_.run_sync<ps::CommitOutcome>(
      [st, loaded = loaded_](ps::IStore& s, const ps::Actor& a) { return entry::save_settings(s, a, st, loaded); });
  if (!saved) return say(message_, QString::fromStdString(to_string(saved.error())), true);
  if (!std::holds_alternative<ps::Committed>(*saved))
    return say(message_, tr("Someone saved the settings meanwhile; close and open them again"), true);
  bridge_.notify_changed();
  QDialog::accept();
}

}  // namespace pychron::ui
