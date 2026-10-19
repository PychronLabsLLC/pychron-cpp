#include "field_table_editor_window.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string_view>
#include <utility>

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QToolBar>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include "field_table_dialogs.hpp"
#include "menu_hub.hpp"
#include "pychron/systems/spectrometer/mftable_csv.hpp"
#include "spectrometer_bridge.hpp"

namespace pychron::ui {

namespace sp = pychron::spectrometer;
namespace fs = std::filesystem;

namespace {

constexpr std::array kFits{sp::FitKind::Discrete, sp::FitKind::Linear, sp::FitKind::Quadratic, sp::FitKind::Cubic};

std::optional<std::string> read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

QString q(const std::string& s) { return QString::fromStdString(s); }

std::vector<std::string> detector_names(const sp::cfg::SpectrometerConfig& config) {
  std::vector<std::string> out;
  out.reserve(config.detectors.size());
  for (const auto& d : config.detectors) out.push_back(d.name);
  return out;
}

QString sv(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString fit_name(sp::FitKind kind) { return sv(to_string(kind)); }

constexpr int kPointRole = Qt::UserRole;
constexpr int kDetectorRole = Qt::UserRole + 1;

}  // namespace

FieldTableEditorWindow::FieldTableEditorWindow(SpectrometerBridge* bridge, fs::path data_root,
                                               sp::cfg::SpectrometerConfig config, sp::MolecularWeights weights,
                                               std::function<QDateTime()> wall_clock, QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      data_root_(std::move(data_root)),
      config_(std::move(config)),
      wall_clock_(std::move(wall_clock)),
      model_(detector_names(config_), std::move(weights)),
      tables_(new QListWidget),
      versions_(new QTreeWidget),
      grid_(new QTableView),
      banner_(new QLabel),
      load_error_label_(new QLabel),
      default_fit_(new QComboBox),
      overrides_(new QFormLayout),
      axis_(new QLabel),
      diagnostics_list_(new QListWidget),
      plot_(new FieldTablePlot) {
  resize(1100, 720);
  tables_->setObjectName(QStringLiteral("tables"));
  versions_->setObjectName(QStringLiteral("versions"));
  grid_->setObjectName(QStringLiteral("grid"));
  banner_->setObjectName(QStringLiteral("banner"));
  load_error_label_->setObjectName(QStringLiteral("load_error"));
  default_fit_->setObjectName(QStringLiteral("default_fit"));
  axis_->setObjectName(QStringLiteral("axis"));
  diagnostics_list_->setObjectName(QStringLiteral("diagnostics"));

  versions_->setColumnCount(3);
  versions_->setHeaderLabels({tr("Version"), tr("Points"), tr("Detectors")});
  versions_->setRootIsDecorated(false);
  versions_->setSelectionMode(QAbstractItemView::ExtendedSelection);  // two to compare

  grid_->setModel(&model_);
  grid_->setSelectionBehavior(QAbstractItemView::SelectItems);
  grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

  banner_->setWordWrap(true);
  banner_->hide();
  load_error_label_->setWordWrap(true);
  load_error_label_->hide();

  for (const sp::FitKind k : kFits) default_fit_->addItem(fit_name(k), static_cast<int>(k));

  // Left: tables, versions of the selected one.
  auto* left = new QWidget;
  auto* left_col = new QVBoxLayout(left);
  left_col->addWidget(new QLabel(tr("Tables")));
  left_col->addWidget(tables_, 1);
  left_col->addWidget(new QLabel(tr("Versions")));
  left_col->addWidget(versions_, 2);

  // Centre: the grid.
  auto* centre = new QWidget;
  auto* centre_col = new QVBoxLayout(centre);
  centre_col->addWidget(banner_);
  centre_col->addWidget(load_error_label_);
  auto* centre_split = new QSplitter(Qt::Vertical);
  centre_split->addWidget(grid_);
  centre_split->addWidget(plot_);
  centre_split->setStretchFactor(0, 3);
  centre_split->setStretchFactor(1, 2);
  centre_col->addWidget(centre_split, 1);
  centre_layout_ = centre_col;

  // Right: fits, axis, diagnostics.
  auto* right = new QWidget;
  auto* right_col = new QVBoxLayout(right);
  auto* fits = new QFormLayout;
  fits->addRow(tr("Default fit"), default_fit_);
  right_col->addLayout(fits);
  right_col->addWidget(new QLabel(tr("Detector fits")));
  right_col->addLayout(overrides_);
  right_col->addWidget(axis_);
  right_col->addWidget(new QLabel(tr("Diagnostics")));
  right_col->addWidget(diagnostics_list_, 1);

  auto* split = new QSplitter;
  split->addWidget(left);
  split->addWidget(centre);
  split->addWidget(right);
  split->setStretchFactor(1, 1);
  split->setSizes({220, 620, 260});
  setCentralWidget(split);

  // Toolbar.
  toolbar_ = addToolBar(tr("Field Table"));
  toolbar_->setObjectName(QStringLiteral("field_table_tools"));
  add_row_action_ = toolbar_->addAction(tr("Add Row"), this, [this] {
    auto d = make_add_row_dialog();
    if (d->exec() == QDialog::Accepted) apply_add_row(*d);
  });
  remove_row_action_ = toolbar_->addAction(tr("Remove Row"), this, [this] {
    std::vector<int> rows;
    for (const QModelIndex& i : grid_->selectionModel()->selectedIndexes()) rows.push_back(i.row());
    model_.remove_rows(std::move(rows));
  });
  set_value_action_ = toolbar_->addAction(tr("Set Value…"), this, [this] {
    auto d = make_set_value_dialog();
    if (d->exec() == QDialog::Accepted) apply_set_value(*d);
  });
  toolbar_->addSeparator();
  save_action_ = toolbar_->addAction(tr("Save"), this, [this] { save(); });
  revert_action_ = toolbar_->addAction(tr("Revert"), this, [this] { revert(); });
  make_current_action_ = toolbar_->addAction(tr("Make Current"), this, [this] {
    if (auto* item = versions_->currentItem()) make_current(item->data(0, Qt::UserRole).toString());
  });
  compare_action_ = toolbar_->addAction(tr("Compare…"), this, [this] {
    // Two selected: older against newer. Else the open edits, or the
    // selected version, against the current one.
    QStringList picked;
    for (const auto* item : versions_->selectedItems()) picked.append(item->data(0, Qt::UserRole).toString());
    const QStringList all = versions();  // newest first
    std::ranges::sort(picked, [&all](const QString& a, const QString& b) { return all.indexOf(a) > all.indexOf(b); });
    if (picked.size() >= 2) {
      compare(picked.front(), picked.back());
    } else if (modified() || picked.isEmpty()) {
      compare(store_current(), QString());
    } else {
      compare(picked.front(), store_current());
    }
  });
  import_action_ = toolbar_->addAction(tr("Import CSV…"), this, [this] { import_csv(); });
  toolbar_->addSeparator();
  undo_action_ = model_.undo_stack()->createUndoAction(this, tr("&Undo"));
  undo_action_->setShortcuts(QKeySequence::Undo);
  redo_action_ = model_.undo_stack()->createRedoAction(this, tr("&Redo"));
  // Ctrl+Shift+Z on every platform, with the platform's own keys.
  QList<QKeySequence> redo_keys = QKeySequence::keyBindings(QKeySequence::Redo);
  const QKeySequence shift_z(Qt::CTRL | Qt::SHIFT | Qt::Key_Z);
  if (!redo_keys.contains(shift_z)) redo_keys.append(shift_z);
  redo_action_->setShortcuts(redo_keys);
  addAction(undo_action_);
  addAction(redo_action_);
  toolbar_->addAction(undo_action_);
  toolbar_->addAction(redo_action_);

  // File > Save while this window is in front; the key is the menu's.
  auto* menu_save = new QAction(tr("&Save"), this);
  MenuHub::instance().set_file_action(this, MenuHub::FileRole::Save, menu_save, tr("Field Table"));
  connect(menu_save, &QAction::triggered, this, [this] { save(); });

  ask_unsaved_ = [this](const QString& what) {
    const auto b = QMessageBox::question(this, tr("Unsaved field table"), tr("%1 has unsaved edits. Save them?").arg(what),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                         QMessageBox::Cancel);
    return b == QMessageBox::Save ? Unsaved::Save : b == QMessageBox::Discard ? Unsaved::Discard : Unsaved::Cancel;
  };
  ask_conflict_ = [this](const QString& now) {
    QMessageBox box(QMessageBox::Warning, tr("Field table"), tr("table changed since you opened it (now %1)").arg(now),
                    QMessageBox::NoButton, this);
    auto* reload = box.addButton(tr("Reload"), QMessageBox::DestructiveRole);
    auto* anyway = box.addButton(tr("Save Anyway"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == reload) return Conflict::Reload;
    if (box.clickedButton() == anyway) return Conflict::SaveAnyway;
    return Conflict::Cancel;
  };
  confirm_ = [this](const QString& question) {
    return QMessageBox::question(this, tr("Field table"), question) == QMessageBox::Yes;
  };
  show_error_ = [this](const QString& error) { QMessageBox::warning(this, tr("Field table"), error); };
  pick_csv_ = [this]() -> std::optional<fs::path> {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Import legacy mftable"), QString(), tr("mftable (*.csv);;All files (*)"));
    if (path.isEmpty()) return std::nullopt;
    return fs::path(path.toStdString());
  };

  check_timer_.setSingleShot(true);
  check_timer_.setInterval(kCheckDelayMs);
  connect(&check_timer_, &QTimer::timeout, this, [this] { check_now(); });
  hold_timer_.setInterval(kHoldPollMs);
  connect(&hold_timer_, &QTimer::timeout, this, [this] { update_actions(); });
  if (bridge_ != nullptr) hold_timer_.start();

  connect(&model_, &FieldTableModel::tableEdited, this, [this] {
    sync_fit_combos();
    check_timer_.start();
    update_actions();
  });
  connect(&model_, &FieldTableModel::tableEdited, this, [this] { redraw_plot(); });
  connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& at) {
    if (!model_.detector_at(at.column()).empty()) redraw_plot();
  });
  connect(model_.undo_stack(), &QUndoStack::cleanChanged, this, [this] {
    update_actions();
    update_title();
  });
  connect(grid_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { update_actions(); });

  connect(default_fit_, &QComboBox::activated, this, [this](int i) {
    model_.set_default_fit(static_cast<sp::FitKind>(default_fit_->itemData(i).toInt()));
  });
  connect(tables_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    const QString name = item->data(Qt::UserRole).toString();
    if (name != open_name_) open_table(name);
    if (open_name_ != name) fill_tables();  // put the highlight back
  });
  connect(versions_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
    const QString v = item->data(0, Qt::UserRole).toString();
    if (v != open_version_) open_version(open_name_, v);
    if (open_version_ != v) fill_versions();
  });
  connect(versions_, &QTreeWidget::currentItemChanged, this, [this] { update_actions(); });
  connect(diagnostics_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    const int row = item->data(kPointRole).toInt();
    const std::string det = item->data(kDetectorRole).toString().toStdString();
    int col = det.empty() ? FieldTableModel::kIsotopeColumn : model_.column_of(det);
    if (col < 0) col = FieldTableModel::kIsotopeColumn;
    const QModelIndex at = model_.index(std::max(row, 0), col);
    if (!at.isValid()) return;
    grid_->setCurrentIndex(at);
    grid_->scrollTo(at);
  });

  if (bridge_ != nullptr) {
    connect(bridge_, &SpectrometerBridge::fieldTableChanged, this,
            [this](const QString& name, const QString& version) { on_table_changed(name, version); });
    connect(bridge_, &SpectrometerBridge::tableSaved, this,
            [this](const QString& name, const Result<std::string>& result) {
              if (save_pending_) finish_save(name, result);
            });
    connect(bridge_, &SpectrometerBridge::tableRestored, this,
            [this](const QString& name, const QString& version, const Result<void>& result) {
              finish_restore(name, version, result);
            });
  }

  fill_tables();
  const QStringList names = table_names();
  const QString active =
      bridge_ != nullptr ? bridge_->active_table() : QString::fromStdString(config_.magnet.field_table);
  if (names.contains(active)) {
    load_current(active);
  } else if (!names.isEmpty()) {
    load_current(names.front());
  }
  update_title();
}

FieldTableEditorWindow::~FieldTableEditorWindow() = default;

sp::FieldTableStore FieldTableEditorWindow::store(const QString& name) const {
  return sp::FieldTableStore(data_root_ / "tables", name.toStdString());
}

fs::path FieldTableEditorWindow::version_file(const QString& name, const QString& version) const {
  return data_root_ / "tables" / name.toStdString() / (version.toStdString() + ".toml");
}

QString FieldTableEditorWindow::store_current() const {
  if (open_name_.isEmpty()) return {};
  auto v = store(open_name_).current_version();
  return v ? q(*v) : QString();
}

std::optional<QString> FieldTableEditorWindow::held() const {
  return bridge_ != nullptr ? bridge_->table_held() : std::nullopt;
}

QStringList FieldTableEditorWindow::table_names() const {
  QStringList out;
  std::error_code ec;
  const fs::path dir = data_root_ / "tables";
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) && fs::exists(it->path() / "current", ec)) out.append(q(it->path().filename().string()));
  }
  out.sort();
  return out;
}

QStringList FieldTableEditorWindow::versions() const {
  QStringList out;
  if (open_name_.isEmpty()) return out;
  auto all = store(open_name_).versions();
  if (!all) return out;
  for (const auto& v : std::views::reverse(*all)) out.append(q(v));
  return out;
}

QStringList FieldTableEditorWindow::diagnostic_lines() const {
  QStringList out;
  for (const auto& d : diagnostics_) out.append(q(sp::to_string(d)));
  return out;
}

bool FieldTableEditorWindow::resolve_unsaved(std::function<void()> then) {
  if (!modified()) return true;
  switch (ask_unsaved_(QStringLiteral("%1 %2").arg(open_name_, open_version_))) {
    case Unsaved::Cancel: return false;
    case Unsaved::Discard: return true;
    case Unsaved::Save:
      after_save_ = std::move(then);
      save();
      return false;
  }
  return false;
}

void FieldTableEditorWindow::open_table(const QString& name) {
  if (resolve_unsaved([this, name] { load_current(name); })) load_current(name);
}

void FieldTableEditorWindow::open_version(const QString& name, const QString& version) {
  if (name == open_name_ && version == open_version_) return;
  if (resolve_unsaved([this, name, version] { load_version(name, version); })) load_version(name, version);
}

void FieldTableEditorWindow::load_current_saved(const QString& name) {
  auto current = store(name).current_version();
  load_version(name, current ? q(*current) : QString());
}

void FieldTableEditorWindow::load_current(const QString& name) {
  auto current = store(name).current_version();
  if (!current) {
    load_version(name, QString());
    load_error_ = q(current.error().what);
    load_error_label_->setText(load_error_);
    load_error_label_->show();
    update_actions();
    return;
  }
  const QString version = q(*current);
  load_version(name, version);
  if (bridge_ == nullptr || model_.read_only()) return;
  // The spectrometer's own copy, when it differs from the store (a save that
  // failed, or a store another process moved on): shown as an edit, so that
  // saving makes it a version.
  sp::FieldTable copy = bridge_->table_copy(name);
  if (copy.points().empty()) return;
  auto stored = store(name).load(*current);
  if (stored && copy == *stored) return;
  model_.edit_table(std::move(copy), tr("In-memory table"));
  set_banner(tr("in-memory table differs from saved version %1").arg(version));
}

void FieldTableEditorWindow::load_version(const QString& name, const QString& version) {
  open_name_ = name;
  open_version_ = version;
  base_version_ = store_current();
  load_error_.clear();
  has_comments_ = false;
  set_banner(QString());

  std::optional<std::string> text;
  if (!version.isEmpty()) text = read_text(version_file(name, version));
  Result<sp::FieldTable> parsed = text ? sp::parse_field_table(*text)
                                       : Result<sp::FieldTable>(fail(ErrorKind::Io, "cannot read version " +
                                                                                       version.toStdString()));
  if (parsed) {
    model_.set_read_only(false);
    has_comments_ = *text != sp::to_toml(*parsed);
    model_.set_table(std::move(*parsed));
    load_error_label_->hide();
  } else {
    model_.set_table(sp::FieldTable{});
    model_.set_read_only(true);
    load_error_ = q(parsed.error().what);
    load_error_label_->setText(tr("version does not parse: %1").arg(load_error_));
    load_error_label_->show();
  }
  axis_->setText(tr("Axis: %1").arg(model_.read_only() ? QString() : sv(to_string(model_.table().axis()))));
  rebuild_fit_combos();
  fill_tables();
  fill_versions();
  check_now();
  update_title();
}

void FieldTableEditorWindow::fill_tables() {
  const QSignalBlocker block(tables_);
  tables_->clear();
  const QString active =
      bridge_ != nullptr ? bridge_->active_table() : QString::fromStdString(config_.magnet.field_table);
  const QString hv = QString::fromStdString(config_.magnet.hv_table);
  for (const QString& name : table_names()) {
    QString text = name;
    if (name == active) text += tr(" (active)");
    if (!hv.isEmpty() && name == hv) text += tr(" (HV)");
    auto* item = new QListWidgetItem(text, tables_);
    item->setData(Qt::UserRole, name);
    if (name == open_name_) tables_->setCurrentItem(item);
  }
}

void FieldTableEditorWindow::fill_versions() {
  const QString selected =
      versions_->currentItem() != nullptr ? versions_->currentItem()->data(0, Qt::UserRole).toString() : QString();
  const QSignalBlocker block(versions_);
  versions_->clear();
  listed_current_ = store_current();
  QTreeWidgetItem* keep = nullptr;
  QTreeWidgetItem* open = nullptr;
  for (const QString& v : versions()) {
    auto* item = new QTreeWidgetItem(versions_);
    item->setData(0, Qt::UserRole, v);
    item->setText(0, v == listed_current_ ? tr("%1 (current)").arg(v) : v);
    const auto text = read_text(version_file(open_name_, v));
    auto parsed = text ? sp::parse_field_table(*text) : Result<sp::FieldTable>(fail(ErrorKind::Io, "unreadable"));
    if (parsed) {
      item->setText(1, QString::number(parsed->points().size()));
      item->setText(2, QString::number(parsed->detectors().size()));
    } else {
      item->setText(1, QStringLiteral("—"));
      item->setText(2, tr("does not parse"));
      item->setToolTip(0, q(parsed.error().what));
    }
    if (v == selected) keep = item;
    if (v == open_version_) open = item;
  }
  if (keep != nullptr) {
    versions_->setCurrentItem(keep);
  } else if (open != nullptr) {
    versions_->setCurrentItem(open);
  }
  update_actions();
}

void FieldTableEditorWindow::rebuild_fit_combos() {
  while (overrides_->rowCount() > 0) overrides_->removeRow(0);
  for (const std::string& det : model_.detector_columns()) {
    auto* combo = new QComboBox;
    combo->setObjectName(q("fit_" + det));
    combo->addItem(tr("default"), -1);
    for (const sp::FitKind k : kFits) combo->addItem(fit_name(k), static_cast<int>(k));
    connect(combo, &QComboBox::activated, this, [this, combo, det](int i) {
      const int kind = combo->itemData(i).toInt();
      model_.set_detector_fit(det, kind < 0 ? std::nullopt : std::optional(static_cast<sp::FitKind>(kind)));
    });
    overrides_->addRow(q(det), combo);
  }
  sync_fit_combos();
}

void FieldTableEditorWindow::sync_fit_combos() {
  const auto& table = model_.table();
  {
    const QSignalBlocker block(default_fit_);
    default_fit_->setCurrentIndex(default_fit_->findData(static_cast<int>(table.default_fit())));
    default_fit_->setEnabled(!model_.read_only());
  }
  for (int r = 0; r < overrides_->rowCount(); ++r) {
    auto* item = overrides_->itemAt(r, QFormLayout::FieldRole);
    auto* combo = item != nullptr ? qobject_cast<QComboBox*>(item->widget()) : nullptr;
    if (combo == nullptr) continue;
    const std::string det = combo->objectName().mid(4).toStdString();
    const auto it = table.fit_overrides().find(det);
    const QSignalBlocker block(combo);
    combo->setCurrentIndex(combo->findData(it == table.fit_overrides().end() ? -1 : static_cast<int>(it->second)));
    combo->setEnabled(!model_.read_only());
  }
}

void FieldTableEditorWindow::check_now() {
  check_timer_.stop();
  if (model_.read_only() || open_name_.isEmpty()) {
    diagnostics_.clear();
  } else {
    diagnostics_ = sp::check_field_table(model_.table(), config_, model_.weights(), open_name_.toStdString());
  }
  diagnostics_list_->clear();
  for (const auto& d : diagnostics_) {
    auto* item = new QListWidgetItem(q(sp::to_string(d)), diagnostics_list_);
    item->setData(kPointRole, d.point ? static_cast<int>(*d.point) : -1);
    item->setData(kDetectorRole, q(d.detector));
  }
  update_actions();
}

QString FieldTableEditorWindow::save_disabled_reason() const {
  if (open_name_.isEmpty()) return tr("no table open");
  if (model_.read_only()) return tr("version does not parse: %1").arg(load_error_);
  if (const auto holder = held()) return tr("table is held by %1").arg(*holder);
  if (const auto* e = sp::first_error(diagnostics_)) return q(sp::to_string(*e));
  if (save_pending_) return tr("saving");
  if (!modified()) return tr("no changes");
  return {};
}

QString FieldTableEditorWindow::make_current_disabled_reason(const QString& version) const {
  if (open_name_.isEmpty() || version.isEmpty()) return tr("no version selected");
  if (const auto holder = held()) return tr("table is held by %1").arg(*holder);
  if (version == listed_current_) return tr("%1 is already current").arg(version);
  const auto text = read_text(version_file(open_name_, version));
  if (!text) return tr("version does not parse: cannot read %1").arg(version);
  auto parsed = sp::parse_field_table(*text);
  if (!parsed) return tr("version does not parse: %1").arg(q(parsed.error().what));
  const auto d = sp::check_field_table(*parsed, config_, model_.weights(), open_name_.toStdString());
  if (const auto* e = sp::first_error(d)) return q(sp::to_string(*e));
  return {};
}

void FieldTableEditorWindow::update_actions() {
  const QString why = save_disabled_reason();
  save_action_->setEnabled(why.isEmpty());
  save_action_->setToolTip(why.isEmpty() ? tr("Save the edits as a new current version") : why);
  revert_action_->setEnabled(modified());
  remove_row_action_->setEnabled(!model_.read_only() && grid_->selectionModel()->hasSelection());
  add_row_action_->setEnabled(!model_.read_only() && !open_name_.isEmpty());
  set_value_action_->setEnabled(!model_.read_only() && !model_.table().points().empty());
  import_action_->setEnabled(!open_name_.isEmpty());
  compare_action_->setEnabled(!open_name_.isEmpty());
  const QString selected =
      versions_->currentItem() != nullptr ? versions_->currentItem()->data(0, Qt::UserRole).toString() : QString();
  const QString mc = make_current_disabled_reason(selected);
  make_current_action_->setEnabled(mc.isEmpty());
  make_current_action_->setToolTip(mc.isEmpty() ? tr("Make %1 the current version").arg(selected) : mc);
}

void FieldTableEditorWindow::update_title() {
  setWindowTitle(open_name_.isEmpty() ? tr("Field Table")
                                      : tr("Field Table — %1 %2[*]").arg(open_name_, open_version_));
  setWindowModified(modified());
}

void FieldTableEditorWindow::set_banner(const QString& text) {
  banner_text_ = text;
  banner_->setText(text);
  banner_->setVisible(!text.isEmpty());
}

// The grid's current detector column; else the one drawn last, while the
// table still has that column; else the first detector.
void FieldTableEditorWindow::redraw_plot() {
  std::string det = model_.detector_at(grid_->currentIndex().column());
  if (det.empty() && model_.column_of(plot_->detector()) >= 0) det = plot_->detector();
  if (det.empty() && !model_.detector_columns().empty()) det = model_.detector_columns().front();
  plot_->show_detector(model_.table(), det);
}

void FieldTableEditorWindow::revert() { load_version(open_name_, open_version_); }

void FieldTableEditorWindow::save() {
  check_now();
  if (const QString why = save_disabled_reason(); !why.isEmpty()) {
    after_save_ = nullptr;
    show_error_(why);
    return;
  }
  const QString key = open_name_ + QLatin1Char('/') + open_version_;
  if (has_comments_ && !comments_asked_.contains(key)) {
    if (!confirm_(tr("comments in %1 are not kept").arg(open_version_))) {
      after_save_ = nullptr;
      return;
    }
    comments_asked_.insert(key);
  }
  if (const QString now = store_current(); now != base_version_) {
    on_conflict(now);
    return;
  }
  start_save(base_version_);
}

void FieldTableEditorWindow::on_conflict(const QString& now) {
  switch (ask_conflict_(now)) {
    case Conflict::Reload:
      after_save_ = nullptr;
      load_version(open_name_, now);
      return;
    case Conflict::SaveAnyway: start_save(QString()); return;
    case Conflict::Cancel: after_save_ = nullptr; return;
  }
}

void FieldTableEditorWindow::start_save(const QString& base) {
  if (bridge_ != nullptr) {
    save_pending_ = true;
    update_actions();
    bridge_->replace_table(open_name_, model_.table(), base);
    return;
  }
  const auto ms = std::chrono::milliseconds(wall_clock_().toMSecsSinceEpoch());
  const sp::FieldTableStore::WallTime when{std::chrono::duration_cast<sp::FieldTableStore::WallTime::duration>(ms)};
  auto store_ = store(open_name_);
  save_pending_ = true;
  finish_save(open_name_, store_.save(model_.table(), when));
}

void FieldTableEditorWindow::finish_save(const QString& name, const Result<std::string>& result) {
  save_pending_ = false;
  std::function<void()> then = std::exchange(after_save_, nullptr);
  if (!result) {
    if (result.error().code == "table_conflict" && name == open_name_) {
      on_conflict(store_current());
    } else {
      show_error_(q(result.error().what));
    }
    update_actions();
    return;
  }
  const QString version = q(*result);
  if (name == open_name_) {
    model_.mark_saved();
    open_version_ = version;
    base_version_ = version;
    has_comments_ = false;
    set_banner(QString());
    fill_versions();
    update_title();
  }
  emit saved(name, version);
  // Another writer may have moved the table on while this save was out.
  if (name == open_name_) {
    if (const QString now = store_current(); !now.isEmpty() && now != version) on_table_changed(name, now);
  }
  if (then) QTimer::singleShot(0, this, std::move(then));
  update_actions();
}

void FieldTableEditorWindow::make_current(const QString& version) {
  if (const QString why = make_current_disabled_reason(version); !why.isEmpty()) {
    show_error_(why);
    return;
  }
  const QString now = store_current();
  if (now != listed_current_) {
    show_error_(tr("table changed since you opened it (now %1)").arg(now));
    fill_versions();
    return;
  }
  if (!confirm_(tr("Make %1 current in place of %2?").arg(version, now))) return;
  if (bridge_ != nullptr) {
    bridge_->restore_table_version(open_name_, version);
    return;
  }
  auto s = store(open_name_);
  finish_restore(open_name_, version, s.restore(version.toStdString()));
}

void FieldTableEditorWindow::finish_restore(const QString& name, const QString& version, const Result<void>& result) {
  if (name != open_name_) return;
  fill_versions();
  if (!result) {
    show_error_(q(result.error().what));
    return;
  }
  if (!modified() && open_version_ != version) load_version(name, version);
}

void FieldTableEditorWindow::on_table_changed(const QString& name, const QString& version) {
  if (name != open_name_ || save_pending_) return;
  fill_versions();
  if (version == open_version_ && version == base_version_) return;
  if (!modified()) {
    load_version(name, version);
  } else {
    set_banner(tr("table changed since you opened it (now %1)").arg(version));
  }
}

QDialog* FieldTableEditorWindow::compare(const QString& v1, const QString& v2) {
  if (open_name_.isEmpty() || v1.isEmpty()) return nullptr;
  const auto side = [this](const QString& v) -> std::pair<std::string, Result<sp::FieldTable>> {
    if (v.isEmpty()) return {sp::to_toml(model_.table()), model_.table()};
    auto text = read_text(version_file(open_name_, v));
    if (!text) return {std::string(), Result<sp::FieldTable>(fail(ErrorKind::Io, "cannot read " + v.toStdString()))};
    auto parsed = sp::parse_field_table(*text);
    return {*text, std::move(parsed)};
  };
  auto [old_text, old_table] = side(v1);
  auto [new_text, new_table] = side(v2);
  const QString new_title = v2.isEmpty() ? tr("open edits") : v2;
  QDialog* d = nullptr;
  if (old_table && new_table) {
    d = new FieldTableCompareDialog(sp::diff_field_tables(*old_table, *new_table), v1, new_title, this);
  } else {
    d = make_text_compare_dialog(v1, q(old_text), new_title, q(new_text), this);
  }
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->show();
  return d;
}

void FieldTableEditorWindow::import_csv() {
  if (open_name_.isEmpty()) return;
  const auto path = pick_csv_();
  if (!path) return;
  const auto text = read_text(*path);
  if (!text) {
    show_error_(tr("cannot read %1").arg(q(path->string())));
    return;
  }
  const QString name = open_name_;
  const auto import = [this, name, text = *text, file = q(path->filename().string())] {
    load_current_saved(name);
    if (model_.read_only()) {
      show_error_(tr("%1 has no current version to import over").arg(name));
      return;
    }
    auto table = sp::read_mftable_csv(text, model_.weights(), model_.table().axis());
    if (!table) {
      show_error_(tr("%1: %2").arg(file, q(table.error().what)));
      return;
    }
    model_.edit_table(std::move(*table), tr("Import CSV"));
  };
  if (resolve_unsaved(import)) import();
}

std::unique_ptr<FieldTableSetValueDialog> FieldTableEditorWindow::make_set_value_dialog() {
  std::vector<std::string> isotopes;
  for (const auto& p : model_.table().points()) isotopes.push_back(p.isotope);
  auto d = std::make_unique<FieldTableSetValueDialog>(model_.detector_columns(), isotopes, config_.magnet.propagate,
                                                      this);
  // Starts at the grid's current cell.
  const QModelIndex at = grid_->currentIndex();
  if (const std::string det = model_.detector_at(at.column()); !det.empty()) d->set_detector(det);
  if (at.isValid() && at.row() < model_.rowCount()) {
    const auto& point = model_.table().points()[static_cast<std::size_t>(at.row())];
    d->set_isotope(point.isotope);
    if (const auto it = point.values.find(d->detector()); it != point.values.end()) d->set_value(it->second);
  }
  return d;
}

void FieldTableEditorWindow::apply_set_value(const FieldTableSetValueDialog& dialog) {
  const auto value = dialog.value();
  if (!value) {
    show_error_(tr("value is not a finite number"));
    return;
  }
  if (auto r = model_.set_value(dialog.detector(), dialog.isotope(), *value, dialog.propagate()); !r) {
    show_error_(q(r.error().what));
  }
}

std::unique_ptr<FieldTableAddRowDialog> FieldTableEditorWindow::make_add_row_dialog() {
  std::vector<std::string> names;
  for (const auto& [name, mass] : model_.weights().entries()) names.push_back(name);
  return std::make_unique<FieldTableAddRowDialog>(names, this);
}

void FieldTableEditorWindow::apply_add_row(const FieldTableAddRowDialog& dialog) {
  if (dialog.isotope().isEmpty()) return;
  model_.add_row(dialog.isotope());
}

void FieldTableEditorWindow::closeEvent(QCloseEvent* event) {
  if (!resolve_unsaved([this] { close(); })) {
    event->ignore();
    return;
  }
  // Discarded edits are gone the next time the window shows.
  if (modified()) load_version(open_name_, open_version_);
  QMainWindow::closeEvent(event);
}

void FieldTableEditorWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::ActivationChange && isActiveWindow() && !open_name_.isEmpty()) {
    fill_tables();
    fill_versions();
  }
  QMainWindow::changeEvent(event);
}

}  // namespace pychron::ui
