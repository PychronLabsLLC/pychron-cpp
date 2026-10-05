#include "packages_window.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTimeZone>
#include <QTableView>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "holder_view.hpp"
#include "level_sheet_pdf.hpp"
#include "package_dialogs.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace ps = persistence;

namespace {

constexpr int kUuidRole = Qt::UserRole;
constexpr int kIsLevelRole = Qt::UserRole + 1;

QString local_text(const ps::UtcTime& t) {
  const QDateTime dt = QDateTime::fromString(QString::fromStdString(t.iso()), Qt::ISODateWithMs);
  return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

std::optional<ps::UtcTime> utc_of(const QString& local) {
  QDateTime dt = QDateTime::fromString(local.trimmed(), QStringLiteral("yyyy-MM-dd HH:mm"));
  if (!dt.isValid()) return std::nullopt;
  dt.setTimeZone(QTimeZone::systemTimeZone());
  return ps::UtcTime::parse(dt.toUTC().toString(Qt::ISODate).toStdString());
}

}  // namespace

struct PackagesWindow::LevelData {
  ps::IrradiationRow package;
  std::optional<ps::LevelSheet> sheet;
  std::vector<ps::RefObjectRow> holders;
  std::map<ps::Uuid, ps::HolderValue> holder_values;
  std::vector<entry::NamedProduction> productions;
  entry::PackageChronology chronology;
  entry::CatalogSnapshot catalog;
  entry::EntrySettings settings;
};

PackagesWindow::PackagesWindow(EntryBridge& bridge, QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), grid_(new LevelGridModel(this)) {
  setWindowTitle(tr("Packages"));
  confirm_ = [this](const QString& question) {
    return QMessageBox::question(this, windowTitle(), question) == QMessageBox::Yes;
  };

  auto* split = new QSplitter(Qt::Horizontal, this);
  tree_ = new QTreeWidget(split);
  tree_->setHeaderLabels({tr("Package"), tr("Positions")});
  tree_->setMinimumWidth(180);
  auto* center = new QWidget(split);
  auto* layout = new QVBoxLayout(center);
  table_ = new QTableView(center);
  table_->setModel(grid_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table_, 1);
  message_ = new QLabel(center);
  message_->setWordWrap(true);
  layout->addWidget(message_);
  split->addWidget(tree_);
  split->addWidget(center);
  split->setStretchFactor(1, 1);
  setCentralWidget(split);

  connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
    if (item->data(0, kIsLevelRole).toBool())
      if (auto id = ps::Uuid::parse(item->data(0, kUuidRole).toString().toStdString())) open_level(*id);
  });
  connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
    if (syncing_) return;
    const auto positions = selected_positions();
    holder_view_->set_selected(std::set<int>(positions.begin(), positions.end()));
  });
  connect(grid_, &LevelGridModel::edited, this, [this] {
    std::map<int, std::string> fill;
    if (grid_->has_edit())
      for (const auto& r : grid_->edit().rows())
        if (r.sample) fill[r.position] = r.project;
    holder_view_->set_fill(std::move(fill));
  });

  build_docks();

  auto* bar = addToolBar(tr("Packages"));
  bar->setObjectName(QStringLiteral("packages_toolbar"));
  bar->addAction(tr("Save"), this, [this] { save(); });
  bar->addAction(tr("Revert"), this, [this] { revert(); });
  bar->addSeparator();
  bar->addAction(tr("New Package…"), this, [this] { new_package(); });
  bar->addAction(tr("New Level…"), this, [this] { new_level(); });
  bar->addAction(tr("Generate Identifiers…"), this, [this] { open_identifiers(); });
  bar->addSeparator();
  bar->addAction(tr("Clear Fields…"), this, [this] {
    ClearFieldsDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) clear_fields(dialog.fields());
  });
  bar->addAction(tr("Fill Packets…"), this, [this] {
    bool ok = false;
    const QString first = QInputDialog::getText(this, tr("Fill Packets"), tr("First packet"), QLineEdit::Normal,
                                                QStringLiteral("P1"), &ok);
    if (ok) fill_packets(first);
  });
  bar->addAction(tr("Import Positions…"), this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Positions"), {}, tr("CSV (*.csv *.txt);;All (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return show_message(tr("Cannot read %1").arg(path), true);
    if (auto r = import_positions(QString::fromUtf8(f.readAll())); !r)
      show_message(QString::fromStdString(r.error().what), true);
  });
  bar->addAction(tr("Export CSV…"), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), QStringLiteral("positions.csv"));
    if (path.isEmpty()) return;
    if (auto r = export_csv(path); !r) show_message(QString::fromStdString(r.error().what), true);
  });
  bar->addAction(tr("Save PDF…"), this, [this] {
    const QString path = QFileDialog::getSaveFileName(this, tr("Level sheets"), QStringLiteral("package.pdf"),
                                                      tr("PDF (*.pdf)"));
    if (path.isEmpty()) return;
    if (auto r = save_pdf(path); !r) show_message(QString::fromStdString(r.error().what), true);
  });

  connect(&bridge_, &EntryBridge::changed, this, [this] { reload(); });
  resize(1400, 800);
  reload();
}

void PackagesWindow::build_docks() {
  // Sample picker.
  auto* picker = new QWidget(this);
  auto* pl = new QVBoxLayout(picker);
  pick_pi_ = new QComboBox(picker);
  pick_project_ = new QComboBox(picker);
  pick_search_ = new QLineEdit(picker);
  pick_search_->setPlaceholderText(tr("Search samples"));
  pick_list_ = new QListWidget(picker);
  auto* assign_button = new QPushButton(tr("Assign to selected"), picker);
  pl->addWidget(pick_pi_);
  pl->addWidget(pick_project_);
  pl->addWidget(pick_search_);
  pl->addWidget(pick_list_, 1);
  pl->addWidget(assign_button);
  for (QComboBox* box : {pick_pi_, pick_project_})
    connect(box, &QComboBox::currentIndexChanged, this, [this] { refresh_picker(); });
  connect(pick_search_, &QLineEdit::textChanged, this, [this] { refresh_picker(); });
  const auto assign_current = [this] {
    auto* item = pick_list_->currentItem();
    if (!item) return show_message(tr("Pick a sample first"), true);
    const QString id = item->data(kUuidRole).toString();
    for (const auto& s : catalog_.samples)
      if (QString::fromStdString(s.uuid.str()) == id) return assign(s);
  };
  connect(assign_button, &QPushButton::clicked, this, assign_current);
  connect(pick_list_, &QListWidget::itemDoubleClicked, this, assign_current);

  // Level.
  auto* level = new QWidget(this);
  auto* form = new QFormLayout(level);
  kind_ = new QComboBox(level);
  kind_->addItem(tr("irradiation"), QStringLiteral("irradiation"));
  kind_->addItem(tr("package"), QStringLiteral("package"));
  holder_ = new QComboBox(level);
  production_ = new QComboBox(level);
  z_ = new QLineEdit(level);
  level_note_ = new QLineEdit(level);
  form->addRow(tr("Package kind"), kind_);
  form->addRow(tr("Holder"), holder_);
  form->addRow(tr("z"), z_);
  form->addRow(tr("Production"), production_);
  auto* edit_production = new QPushButton(tr("Edit Production…"), level);
  form->addRow(QString(), edit_production);
  form->addRow(tr("Note"), level_note_);
  connect(kind_, &QComboBox::activated, this, [this] { set_kind(kind_->currentData().toString()); });
  connect(holder_, &QComboBox::activated, this, [this] {
    if (!grid_->has_edit()) return;
    const auto id = ps::Uuid::parse(holder_->currentData().toString().toStdString());
    grid_->modify([&](entry::LevelSheetEdit& e) { e.set_holder(id, holder_value(id)); });
    holder_view_->set_holder(holder_value(id));
  });
  connect(production_, &QComboBox::activated, this, [this] {
    const auto id = ps::Uuid::parse(production_->currentData().toString().toStdString());
    grid_->modify([&](entry::LevelSheetEdit& e) { e.set_production(id); });
  });
  connect(z_, &QLineEdit::editingFinished, this, [this] {
    const QString t = z_->text().trimmed();
    bool ok = true;
    const double v = t.toDouble(&ok);
    if (!t.isEmpty() && !ok) return show_message(tr("z is a number"), true);
    grid_->modify([&](entry::LevelSheetEdit& e) { e.set_z(t.isEmpty() ? std::nullopt : std::optional<double>(v)); });
  });
  connect(level_note_, &QLineEdit::editingFinished, this, [this] {
    const QString t = level_note_->text().trimmed();
    grid_->modify([&](entry::LevelSheetEdit& e) {
      e.set_level_note(t.isEmpty() ? std::nullopt : std::optional<std::string>(t.toStdString()));
    });
  });
  connect(edit_production, &QPushButton::clicked, this, [this] {
    auto pkg = current_package();
    if (!pkg) return;
    ProductionDialog dialog(bridge_, *pkg, productions_, production_->currentData().toString(), this);
    if (dialog.exec() == QDialog::Accepted) {
      bridge_.notify_changed();
      if (level_) open_level(*level_);
    }
  });

  // Chronology.
  chronology_page_ = new QWidget(this);
  auto* cl = new QVBoxLayout(chronology_page_);
  doses_ = new QTableWidget(0, 3, chronology_page_);
  doses_->setHorizontalHeaderLabels({tr("Power"), tr("Start (local)"), tr("End (local)")});
  doses_->horizontalHeader()->setStretchLastSection(true);
  hours_ = new QLabel(chronology_page_);
  auto* buttons = new QHBoxLayout();
  auto* add = new QPushButton(tr("Add Dose"), chronology_page_);
  auto* remove = new QPushButton(tr("Remove"), chronology_page_);
  auto* save_chronology = new QPushButton(tr("Save Chronology"), chronology_page_);
  buttons->addWidget(add);
  buttons->addWidget(remove);
  buttons->addStretch(1);
  buttons->addWidget(save_chronology);
  cl->addWidget(doses_, 1);
  cl->addWidget(hours_);
  cl->addLayout(buttons);
  connect(add, &QPushButton::clicked, this, [this] {
    const int at = doses_->rowCount();
    // The previous dose's end day, 08:00 to 17:00 (legacy chronology.py:129-138).
    QDate day = QDate::currentDate();
    if (at > 0)
      day = QDateTime::fromString(doses_->item(at - 1, 2)->text(), QStringLiteral("yyyy-MM-dd HH:mm")).date();
    doses_->insertRow(at);
    doses_->setItem(at, 0, new QTableWidgetItem(QStringLiteral("1.0")));
    doses_->setItem(at, 1, new QTableWidgetItem(day.toString(Qt::ISODate) + QStringLiteral(" 08:00")));
    doses_->setItem(at, 2, new QTableWidgetItem(day.toString(Qt::ISODate) + QStringLiteral(" 17:00")));
  });
  connect(remove, &QPushButton::clicked, this, [this] {
    if (doses_->currentRow() >= 0) doses_->removeRow(doses_->currentRow());
  });
  connect(save_chronology, &QPushButton::clicked, this, [this] {
    auto pkg = current_package();
    if (!pkg) return;
    std::vector<ps::Dose> doses;
    for (int r = 0; r < doses_->rowCount(); ++r) {
      bool ok = false;
      const double power = doses_->item(r, 0) ? doses_->item(r, 0)->text().toDouble(&ok) : 0;
      const auto start = doses_->item(r, 1) ? utc_of(doses_->item(r, 1)->text()) : std::nullopt;
      const auto end = doses_->item(r, 2) ? utc_of(doses_->item(r, 2)->text()) : std::nullopt;
      if (!ok || !start || !end) return show_message(tr("Dose %1: power, then yyyy-MM-dd HH:mm times").arg(r + 1), true);
      doses.push_back(ps::Dose{r, power, *start, *end});
    }
    entry::NewPackage check;
    check.name = pkg->name;
    check.doses = doses;
    check.reactor = "-";
    if (auto problems = entry::validate(check, {}); !problems.empty())
      return show_message(QString::fromStdString(problems.front()), true);
    ++busy_;
    bridge_.run<bool>(
        this,
        [pkg = *pkg, doses, loaded = chronology_](ps::IStore& s, const ps::Actor& a) -> Result<bool> {
          if (auto r = entry::save_chronology(s, a, pkg, doses, loaded); !r) return fail(r.error());
          return true;
        },
        [this](Result<bool> r) {
          --busy_;
          if (!r) return show_message(QString::fromStdString(r.error().what), true);
          show_message(tr("Chronology saved"));
          bridge_.notify_changed();
        });
  });

  holder_view_ = new HolderView(this);
  connect(holder_view_, &HolderView::selection_changed, this, [this](const std::set<int>& positions) {
    select_positions(positions);
  });

  auto* tabs = new QTabWidget(this);
  tabs->addTab(picker, tr("Samples"));
  tabs->addTab(level, tr("Level"));
  tabs->addTab(chronology_page_, tr("Chronology"));
  tabs->addTab(holder_view_, tr("Holder"));
  auto* dock = new QDockWidget(tr("Edit"), this);
  dock->setObjectName(QStringLiteral("packages_dock"));
  dock->setWidget(tabs);
  addDockWidget(Qt::RightDockWidgetArea, dock);
}

QString PackagesWindow::message() const { return message_->text(); }

void PackagesWindow::show_message(const QString& text, bool error) {
  message_->setText(text);
  style::set_tone(message_, error ? style::Tone::Error : style::Tone::Normal);
}

bool PackagesWindow::ask(const QString& question) { return confirm_ ? confirm_(question) : false; }

std::optional<ps::IrradiationRow> PackagesWindow::current_package() const {
  if (!package_) return std::nullopt;
  for (const auto& p : packages_)
    if (p.uuid == *package_) return p;
  return std::nullopt;
}

std::optional<ps::HolderValue> PackagesWindow::holder_value(std::optional<ps::Uuid> holder) const {
  if (!holder) return std::nullopt;
  auto it = holder_values_.find(*holder);
  if (it == holder_values_.end()) return std::nullopt;
  return it->second;
}

void PackagesWindow::reload() {
  ++busy_;
  bridge_.run<std::pair<std::vector<ps::IrradiationRow>, std::map<ps::Uuid, std::vector<ps::LevelRow>>>>(
      this,
      [](ps::IStore& s, const ps::Actor&)
          -> Result<std::pair<std::vector<ps::IrradiationRow>, std::map<ps::Uuid, std::vector<ps::LevelRow>>>> {
        auto packages = s.irradiations();
        if (!packages) return fail(packages.error());
        std::map<ps::Uuid, std::vector<ps::LevelRow>> levels;
        for (const auto& p : *packages) {
          auto l = s.levels(p.uuid);
          if (!l) return fail(l.error());
          levels.emplace(p.uuid, std::move(*l));
        }
        return std::make_pair(std::move(*packages), std::move(levels));
      },
      [this](auto r) {
        --busy_;
        if (!r) return show_message(QString::fromStdString(to_string(r.error())), true);
        packages_ = std::move(r->first);
        tree_->clear();
        QTreeWidgetItem* current = nullptr;
        for (const auto& p : packages_) {
          auto* item = new QTreeWidgetItem(tree_);
          item->setText(0, p.kind == "package" ? QStringLiteral("%1 (package)").arg(QString::fromStdString(p.name))
                                               : QString::fromStdString(p.name));
          item->setText(1, QString::number(p.n_positions));
          item->setData(0, kUuidRole, QString::fromStdString(p.uuid.str()));
          item->setData(0, kIsLevelRole, false);
          for (const auto& l : r->second[p.uuid]) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, QString::fromStdString(l.name));
            child->setData(0, kUuidRole, QString::fromStdString(l.uuid.str()));
            child->setData(0, kIsLevelRole, true);
            if (level_ && l.uuid == *level_) current = child;
          }
        }
        if (current) {
          tree_->setCurrentItem(current);
          current->parent()->setExpanded(true);
        }
        show_message(tr("%1 packages").arg(packages_.size()));
      });
}

bool PackagesWindow::settle_edits() {
  if (!grid_->has_edit() || !grid_->edit().dirty()) return true;
  const auto answer = confirm_ ? confirm_(tr("Save the edits of this level first? (No drops them)")) : false;
  if (answer) save();
  return true;
}

void PackagesWindow::open_level(ps::Uuid level) {
  if (!settle_edits()) return;
  ++busy_;
  bridge_.run<LevelData>(
      this,
      [level](ps::IStore& s, const ps::Actor&) -> Result<LevelData> {
        LevelData d;
        auto sheet = s.level_sheet(level);
        if (!sheet) return fail(sheet.error());
        if (!*sheet) return fail(ErrorKind::Config, "the level is gone");
        d.sheet = std::move(**sheet);
        auto packages = s.irradiations();
        if (!packages) return fail(packages.error());
        for (const auto& p : *packages)
          if (p.uuid == d.sheet->level.irradiation) d.package = p;
        auto holders = s.ref_objects(ps::RefType::IrradiationHolder, std::nullopt);
        if (!holders) return fail(holders.error());
        d.holders = std::move(*holders);
        for (const auto& h : d.holders) {
          auto v = entry::load_holder(s, h.uuid);
          if (!v) return fail(v.error());
          if (*v) d.holder_values.emplace(h.uuid, std::move(**v));
        }
        auto productions = entry::package_productions(s, d.package.uuid, d.package.name);
        if (!productions) return fail(productions.error());
        d.productions = std::move(*productions);
        auto chronology = entry::package_chronology(s, d.package.uuid, d.package.name);
        if (!chronology) return fail(chronology.error());
        d.chronology = std::move(*chronology);
        auto catalog = entry::read_snapshot(s);
        if (!catalog) return fail(catalog.error());
        d.catalog = std::move(*catalog);
        auto settings = entry::load_settings(s);
        if (!settings) return fail(settings.error());
        d.settings = settings->settings;
        return d;
      },
      [this](Result<LevelData> r) {
        --busy_;
        if (!r) return show_message(QString::fromStdString(to_string(r.error())), true);
        apply_level(std::move(*r));
      });
}

void PackagesWindow::apply_level(LevelData d) {
  package_ = d.package.uuid;
  level_ = d.sheet->level.uuid;
  for (auto& p : packages_)
    if (p.uuid == d.package.uuid) p = d.package;
  holders_ = std::move(d.holders);
  holder_values_ = std::move(d.holder_values);
  productions_ = std::move(d.productions);
  chronology_ = std::move(d.chronology);
  catalog_ = std::move(d.catalog);
  settings_ = std::move(d.settings);
  const auto holder = holder_value(d.sheet->level.holder);
  grid_->set_edit(entry::LevelSheetEdit(std::move(*d.sheet), holder));
  holder_view_->set_holder(holder);
  holder_view_->set_selected({});
  Q_EMIT grid_->edited();
  fill_level_dock();
  fill_chronology();
  refresh_picker();
  setWindowTitle(tr("Packages — %1 %2")
                     .arg(QString::fromStdString(d.package.name), QString::fromStdString(grid_->edit().loaded().level.name)));
  show_message(tr("%1 holes").arg(grid_->rowCount()));
}

void PackagesWindow::fill_level_dock() {
  const auto pkg = current_package();
  kind_->setCurrentIndex(pkg && pkg->kind == "package" ? 1 : 0);
  holder_->clear();
  holder_->addItem(tr("(none)"), QString());
  for (const auto& h : holders_) holder_->addItem(QString::fromStdString(h.key), QString::fromStdString(h.uuid.str()));
  production_->clear();
  production_->addItem(tr("(none)"), QString());
  for (const auto& p : productions_)
    production_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.ref_object.str()));
  if (!grid_->has_edit()) return;
  const auto& e = grid_->edit();
  const auto index_of = [](QComboBox* box, const std::optional<ps::Uuid>& id) {
    return id ? std::max(0, box->findData(QString::fromStdString(id->str()))) : 0;
  };
  holder_->setCurrentIndex(index_of(holder_, e.holder()));
  production_->setCurrentIndex(index_of(production_, e.production()));
  z_->setText(e.z() ? QString::number(*e.z(), 'g', 10) : QString());
  level_note_->setText(e.level_note() ? QString::fromStdString(*e.level_note()) : QString());
  const bool irradiation = !pkg || pkg->kind == "irradiation";
  production_->setEnabled(irradiation);
  chronology_page_->setEnabled(irradiation);
}

void PackagesWindow::fill_chronology() {
  doses_->setRowCount(0);
  for (const auto& d : chronology_.value.doses) {
    const int at = doses_->rowCount();
    doses_->insertRow(at);
    doses_->setItem(at, 0, new QTableWidgetItem(QString::number(d.power)));
    doses_->setItem(at, 1, new QTableWidgetItem(local_text(d.start)));
    doses_->setItem(at, 2, new QTableWidgetItem(local_text(d.end)));
  }
  const double hours = entry::dose_hours(chronology_.value.doses);
  hours_->setText(tr("%1 h, estimated J %2")
                      .arg(hours, 0, 'f', 2)
                      .arg(entry::estimated_j(chronology_.value.doses, settings_), 0, 'E', 3));
}

void PackagesWindow::refresh_picker() {
  const QString pi = pick_pi_->currentData().toString(), project = pick_project_->currentData().toString();
  // Filters: rebuilt from the catalog, keeping the choice.
  {
    const QSignalBlocker b1(pick_pi_), b2(pick_project_);
    pick_pi_->clear();
    pick_pi_->addItem(tr("Any PI"), QString());
    for (const auto& p : catalog_.principal_investigators)
      pick_pi_->addItem(QString::fromStdString(p.display_name), QString::fromStdString(p.uuid.str()));
    pick_pi_->setCurrentIndex(std::max(0, pick_pi_->findData(pi)));
    pick_project_->clear();
    pick_project_->addItem(tr("Any project"), QString());
    for (const auto& p : catalog_.projects)
      if (pi.isEmpty() || (p.principal_investigator && QString::fromStdString(p.principal_investigator->str()) == pi))
        pick_project_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.uuid.str()));
    pick_project_->setCurrentIndex(std::max(0, pick_project_->findData(project)));
  }
  const QString text = pick_search_->text().trimmed().toLower();
  pick_list_->clear();
  for (const auto& s : catalog_.samples) {
    if (!pi.isEmpty() && (!s.principal_investigator || QString::fromStdString(s.principal_investigator->str()) != pi)) continue;
    if (!project.isEmpty() && QString::fromStdString(s.project.str()) != project) continue;
    if (!text.isEmpty() && !QString::fromStdString(s.name).toLower().contains(text)) continue;
    auto* item = new QListWidgetItem(QStringLiteral("%1  —  %2, %3%4")
                                         .arg(QString::fromStdString(s.name), QString::fromStdString(s.project_name),
                                              QString::fromStdString(s.material_name),
                                              s.grainsize.empty() ? QString()
                                                                  : QStringLiteral(" (%1)").arg(QString::fromStdString(s.grainsize))),
                                     pick_list_);
    item->setData(kUuidRole, QString::fromStdString(s.uuid.str()));
  }
}

std::vector<int> PackagesWindow::selected_positions() const {
  std::vector<int> out;
  for (const QModelIndex& i : table_->selectionModel()->selectedRows())
    if (const int p = grid_->position_at(i.row())) out.push_back(p);
  std::sort(out.begin(), out.end());
  return out;
}

void PackagesWindow::select_positions(const std::set<int>& positions) {
  syncing_ = true;
  table_->clearSelection();
  for (int p : positions)
    if (const int row = grid_->row_of(p); row >= 0)
      table_->selectionModel()->select(grid_->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
  syncing_ = false;
  holder_view_->set_selected(positions);
}

void PackagesWindow::assign(const ps::SampleRow& sample) {
  if (!grid_->has_edit()) return;
  const auto positions = selected_positions();
  if (positions.empty()) return show_message(tr("Select positions first"), true);
  int analyses = 0;
  for (int p : positions)
    if (const auto* r = grid_->edit().row(p); r && r->stored) analyses += r->stored->n_analyses;
  if (analyses > 0 &&
      !ask(tr("%1 analyses were made of these positions; their sample becomes %2. Continue?")
               .arg(analyses)
               .arg(QString::fromStdString(sample.name))))
    return;
  grid_->modify([&](entry::LevelSheetEdit& e) { e.assign_sample(positions, sample); });
  select_positions(std::set<int>(positions.begin(), positions.end()));
}

void PackagesWindow::clear_fields(const std::set<entry::SheetField>& fields) {
  const auto positions = selected_positions();
  grid_->modify([&](entry::LevelSheetEdit& e) { e.clear(positions, fields); });
}

void PackagesWindow::fill_packets(const QString& first) {
  const auto positions = selected_positions();
  Result<void> r;
  grid_->modify([&](entry::LevelSheetEdit& e) { r = e.fill_packets(positions, first.toStdString()); });
  if (!r) show_message(QString::fromStdString(r.error().what), true);
}

void PackagesWindow::revert() {
  if (level_) {
    grid_->set_edit(std::nullopt);
    open_level(*level_);
  }
}

void PackagesWindow::save() {
  if (!grid_->has_edit() || !grid_->edit().dirty()) return show_message(tr("Nothing to save"));
  const auto& edit = grid_->edit();
  if (const auto problems = edit.validate(settings_); !problems.empty())
    return show_message(QString::fromStdString(problems.front()), true);
  auto batch = edit.to_batch();
  if (const int n = edit.analyses_changing_sample(); n > 0) {
    if (!ask(tr("This changes the sample of %1 analyses. Save?").arg(n))) return;
    batch.allow_analyzed_sample_change = true;
  }
  ++busy_;
  bridge_.run<ps::CatalogOutcome>(
      this,
      [edit, batch](ps::IStore& s, const ps::Actor& a) -> Result<ps::CatalogOutcome> {
        auto uow = s.begin(a);
        if (!uow) return fail(uow.error());
        auto staged = edit.stage_refs(**uow);
        if (!staged) return fail(staged.error());
        if (*staged) return s.apply_catalog_edits(a, batch, **uow, ps::ChangesetKind::Reference, batch.message);
        return s.apply_catalog_edits(a.client, batch);
      },
      [this](Result<ps::CatalogOutcome> r) {
        --busy_;
        if (!r) return show_message(QString::fromStdString(to_string(r.error())), true);
        if (std::holds_alternative<ps::CatalogApplied>(*r)) {
          show_message(tr("Saved"));
          grid_->set_edit(std::nullopt);
          bridge_.notify_changed();
          if (level_) open_level(*level_);
          return;
        }
        if (const auto* stale = std::get_if<std::vector<ps::StaleRow>>(&*r))
          return show_message(tr("%1 rows were changed by another client; nothing was saved. Revert and edit again.")
                                  .arg(stale->size()),
                              true);
        if (const auto* refused = std::get_if<std::vector<ps::Refusal>>(&*r)) {
          QStringList lines;
          for (const auto& x : *refused) lines << QString::fromStdString(x.what);
          return show_message(tr("Nothing was saved: %1").arg(lines.join(QStringLiteral("; "))), true);
        }
        show_message(tr("The level's z or production was changed by another client; nothing was saved"), true);
      });
}

Result<int> PackagesWindow::import_positions(const QString& csv) {
  if (!grid_->has_edit()) return fail(ErrorKind::Config, "open a level first");
  auto table = entry::read_csv(csv.toStdString());
  if (!table) return fail(table.error());
  std::map<std::string, entry::LevelSheetEdit> levels;
  levels.emplace(grid_->edit().loaded().level.name, grid_->edit());
  const auto result = entry::apply_position_import(*table, catalog_, levels, settings_.pi_names_allowed);
  if (!result.errors.empty()) {
    std::string what = result.errors.front();
    if (result.errors.size() > 1) what += " (and " + std::to_string(result.errors.size() - 1) + " more)";
    return fail(ErrorKind::Config, what);
  }
  grid_->modify([&](entry::LevelSheetEdit& e) { e = levels.begin()->second; });
  show_message(tr("%1 positions filled; Save writes them").arg(result.applied));
  return result.applied;
}

Result<void> PackagesWindow::export_csv(const QString& path) {
  const auto pkg = current_package();
  if (!pkg) return fail(ErrorKind::Config, "open a level first");
  auto text = bridge_.run_sync<std::string>([id = pkg->uuid](ps::IStore& s, const ps::Actor&) -> Result<std::string> {
    auto sheets = entry::package_sheets(s, id);
    if (!sheets) return fail(sheets.error());
    return entry::export_package_csv(*sheets);
  });
  if (!text) return fail(text.error());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return fail(ErrorKind::Io, "cannot write " + path.toStdString());
  f.write(QByteArray::fromStdString(*text));
  return {};
}

Result<int> PackagesWindow::save_pdf(const QString& path) {
  const auto pkg = current_package();
  if (!pkg) return fail(ErrorKind::Config, "open a level first");
  auto sheets = bridge_.run_sync<PackageSheets>([pkg = *pkg](ps::IStore& s, const ps::Actor&) -> Result<PackageSheets> {
    PackageSheets out;
    out.package = pkg;
    auto levels = entry::package_sheets(s, pkg.uuid);
    if (!levels) return fail(levels.error());
    out.levels = std::move(*levels);
    for (const auto& l : out.levels)
      if (l.level.holder && !out.holders.count(*l.level.holder)) {
        auto h = entry::load_holder(s, *l.level.holder);
        if (!h) return fail(h.error());
        if (*h) out.holders.emplace(*l.level.holder, **h);
      }
    auto chronology = entry::package_chronology(s, pkg.uuid, pkg.name);
    if (!chronology) return fail(chronology.error());
    if (chronology->ref_object) out.chronology = chronology->value;
    return out;
  });
  if (!sheets) return fail(sheets.error());
  auto pages = write_package_pdf(path, *sheets);
  if (pages) show_message(tr("Wrote %1 pages").arg(*pages));
  return pages;
}

void PackagesWindow::set_kind(const QString& kind) {
  const auto pkg = current_package();
  if (!pkg || QString::fromStdString(pkg->kind) == kind) return;
  if (!ask(tr("Make %1 a %2? Its chronology and productions are kept, and %3.")
               .arg(QString::fromStdString(pkg->name), kind,
                    kind == QStringLiteral("package") ? tr("hidden") : tr("shown again"))))
    return fill_level_dock();
  ps::CatalogEditBatch batch;
  batch.edits = {ps::CatalogUpdate{ps::CatalogTable::Irradiation, pkg->uuid, {{"kind", pkg->kind}}, {{"kind", kind.toStdString()}}}};
  ++busy_;
  bridge_.run<ps::CatalogOutcome>(
      this, [batch](ps::IStore& s, const ps::Actor& a) { return s.apply_catalog_edits(a.client, batch); },
      [this, kind](Result<ps::CatalogOutcome> r) {
        --busy_;
        if (!r || !std::holds_alternative<ps::CatalogApplied>(*r)) {
          show_message(tr("The kind was not changed"), true);
          return fill_level_dock();
        }
        for (auto& p : packages_)
          if (package_ && p.uuid == *package_) p.kind = kind.toStdString();
        fill_level_dock();
        bridge_.notify_changed();
      });
}

void PackagesWindow::open_identifiers() {
  const auto pkg = current_package();
  if (!pkg) return show_message(tr("Open a level of the package first"), true);
  if (grid_->has_edit() && grid_->edit().dirty()) return show_message(tr("Save the level first"), true);
  auto* dialog = new IdentifierDialog(bridge_, *pkg, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &IdentifierDialog::allocated, this, [this] {
    bridge_.notify_changed();
    if (level_) open_level(*level_);
  });
  dialog->show();
}

void PackagesWindow::new_package() {
  NewPackageDialog dialog(bridge_, this);
  if (dialog.exec() == QDialog::Accepted) {
    bridge_.notify_changed();
    if (dialog.first_level()) open_level(*dialog.first_level());
  }
}

void PackagesWindow::new_level() {
  const auto pkg = current_package();
  if (!pkg) return show_message(tr("Open a level of the package first"), true);
  std::vector<std::string> names;
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    auto* item = tree_->topLevelItem(i);
    if (item->data(0, kUuidRole).toString() != QString::fromStdString(pkg->uuid.str())) continue;
    for (int c = 0; c < item->childCount(); ++c) names.push_back(item->child(c)->text(0).toStdString());
  }
  entry::NewLevel defaults;
  defaults.name = entry::next_level_name(names);
  std::optional<ps::Uuid> production;
  if (grid_->has_edit()) {
    defaults.holder = grid_->edit().holder();
    defaults.z = grid_->edit().z();
    production = grid_->edit().production();
  }
  NewLevelDialog dialog(bridge_, *pkg, defaults, production, holders_, productions_, this);
  if (dialog.exec() == QDialog::Accepted) {
    bridge_.notify_changed();
    if (dialog.level()) open_level(*dialog.level());
  }
}

void PackagesWindow::closeEvent(QCloseEvent* event) {
  if (grid_->has_edit() && grid_->edit().dirty()) {
    const auto answer = QMessageBox::question(this, windowTitle(), tr("Save the edits of this level?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) return event->ignore();
    if (answer == QMessageBox::Save) save();
  }
  event->accept();
}

}  // namespace pychron::ui
