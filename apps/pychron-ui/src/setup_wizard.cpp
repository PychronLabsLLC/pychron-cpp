#include "setup_wizard.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWizardPage>

#include "pychron/setup/connection.hpp"
#include "pychron/setup/installer.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace fs = std::filesystem;
using namespace pychron::setup;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

QLabel* note(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setWordWrap(true);
  l->setTextFormat(Qt::PlainText);
  return l;
}

QString escaped(const std::string& s) { return qs(s).toHtmlEscaped(); }

// A page whose completeness the wizard decides (the Ready page waits for a
// plan that renders).
class ReadyPage : public QWizardPage {
 public:
  using QWizardPage::QWizardPage;
  std::function<bool()> complete;
  bool isComplete() const override { return complete ? complete() : QWizardPage::isComplete(); }
  void changed() { emit completeChanged(); }
};

constexpr int kChoiceRadioLimit = 4;  // more choices than this: a combo box

}  // namespace

SetupWizard::SetupWizard(const ProfileLibrary& library, Options options, QWidget* parent)
    : QWizard(parent), library_(library), options_(std::move(options)) {
  if (options_.site_path.empty()) options_.site_path = default_site_path();
  setWindowTitle(tr("Set up Pychron"));
  setOption(QWizard::NoBackButtonOnLastPage);
  setOption(QWizard::NoCancelButtonOnLastPage);
  setPage(kWelcome, make_welcome());
  setPage(kLocation, make_location());
  setPage(kReady, make_ready());
  setPage(kDone, make_done());
  setStartId(kWelcome);
  resize(720, 560);
  if (!options_.profile.isEmpty()) choose(options_.profile);
}

// --- pages -----------------------------------------------------------------

QWizardPage* SetupWizard::make_welcome() {
  auto* page = new QWizardPage(this);
  page->setTitle(tr("Welcome to Pychron"));
  page->setSubTitle(tr("Choose what to set up on this computer. You can add more later from File > Installations."));
  auto* layout = new QVBoxLayout(page);
  choices_ = new QButtonGroup(page);
  auto add_section = [&](const QString& heading, ProfileKind kind) {
    bool any = false;
    for (const Profile* p : library_.list()) {
      if (p->kind != kind) continue;
      if (!any) {
        auto* h = new QLabel(QStringLiteral("<b>%1</b>").arg(heading.toHtmlEscaped()), page);
        layout->addWidget(h);
        any = true;
      }
      auto* radio = new QRadioButton(qs(p->title.empty() ? p->name : p->title), page);
      radio->setObjectName(qs("profile-" + p->name));
      choices_->addButton(radio, static_cast<int>(choice_names_.size()));
      choice_names_.push_back(p->name);
      layout->addWidget(radio);
      if (!p->summary.empty()) {
        auto* s = note(qs(p->summary), page);
        s->setContentsMargins(24, 0, 0, 6);
        style::set_tone(s, style::Tone::Muted);
        layout->addWidget(s);
      }
    }
    if (any) layout->addSpacing(8);
  };
  add_section(tr("Reduce and plot data"), ProfileKind::DataReduction);
  add_section(tr("Run an instrument"), ProfileKind::Instrument);
  layout->addStretch(1);
  if (auto* first = choices_->button(0)) first->setChecked(true);
  return page;
}

QWizardPage* SetupWizard::make_location() {
  auto* page = new QWizardPage(this);
  page->setTitle(tr("Install location"));
  page->setSubTitle(tr("Where Pychron keeps this installation's settings and data."));
  auto* form = new QFormLayout(page);
  name_ = new QLineEdit(page);
  name_->setObjectName(QStringLiteral("install-name"));
  form->addRow(tr("Name:"), name_);
  auto* row = new QWidget(page);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  root_ = new QLineEdit(row);
  root_->setObjectName(QStringLiteral("install-root"));
  auto* browse = new QPushButton(tr("Browse…"), row);
  h->addWidget(root_, 1);
  h->addWidget(browse);
  form->addRow(tr("Folder:"), row);
  location_note_ = note(QString(), page);
  form->addRow(QString(), location_note_);
  connect(name_, &QLineEdit::textEdited, this, [this] {
    name_edited_ = true;
    if (!root_edited_ && profile_) root_->setText(qs(default_root(profile_->top, ss(name_->text())).string()));
  });
  connect(root_, &QLineEdit::textEdited, this, [this] { root_edited_ = true; });
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Install folder"), root_->text());
    if (dir.isEmpty()) return;
    root_->setText(dir);
    root_edited_ = true;
  });
  return page;
}

QWizardPage* SetupWizard::make_ready() {
  auto* page = new ReadyPage(this);
  page->setTitle(tr("Ready to install"));
  page->setCommitPage(true);
  page->setButtonText(QWizard::CommitButton, tr("Install"));
  auto* layout = new QVBoxLayout(page);
  summary_ = new QTextBrowser(page);
  summary_->setOpenLinks(false);
  ready_error_ = note(QString(), page);
  style::set_tone(ready_error_, style::Tone::Error);
  ready_error_->setVisible(false);
  layout->addWidget(summary_, 1);
  layout->addWidget(ready_error_);
  page->complete = [this] { return plan_.has_value(); };
  return page;
}

QWizardPage* SetupWizard::make_done() {
  auto* page = new QWizardPage(this);
  page->setTitle(tr("Pychron is set up"));
  auto* layout = new QVBoxLayout(page);
  done_report_ = new QTextBrowser(page);
  done_report_->setOpenLinks(false);
  open_now_ = new QCheckBox(tr("Open it now"), page);
  open_now_->setChecked(true);
  layout->addWidget(done_report_, 1);
  layout->addWidget(open_now_);
  page->setFinalPage(true);
  return page;
}

void SetupWizard::rebuild_groups() {
  for (int id : pageIds()) {
    if (id >= kFirstGroup && id < kReady) {
      QWizardPage* p = page(id);
      removePage(id);
      delete p;
    }
  }
  groups_.clear();
  fields_.clear();
  field_index_.clear();
  test_button_ = nullptr;
  test_result_ = nullptr;
  instrument_test_button_ = nullptr;
  instrument_test_result_ = nullptr;
  if (!profile_) return;
  building_ = true;

  groups_ = profile_->groups;
  std::vector<QFormLayout*> forms;
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    auto* page = new QWizardPage(this);
    page->setTitle(qs(groups_[i]));
    page->setSubTitle(qs(profile_->top.title));
    auto* form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    forms.push_back(form);
    setPage(kFirstGroup + static_cast<int>(i), page);
  }
  fields_.reserve(profile_->questions.size());
  for (const auto& q : profile_->questions) {
    const int group = static_cast<int>(std::find(groups_.begin(), groups_.end(), q.group) - groups_.begin());
    Field f;
    f.question = q;
    f.group = group;
    f.form = forms[static_cast<std::size_t>(group)];
    fields_.push_back(std::move(f));
  }
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    Field& f = fields_[i];
    field_index_[f.question.id] = i;
    QWidget* page = f.form->parentWidget();
    auto* cell = new QWidget(page);
    auto* v = new QVBoxLayout(cell);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    f.editor = make_editor(f);
    f.editor->setParent(cell);
    f.editor->setObjectName(qs("answer-" + f.question.id));
    v->addWidget(f.editor);
    if (!f.question.help.empty()) {
      auto* help = note(qs(f.question.help), cell);
      style::set_tone(help, style::Tone::Muted);
      v->addWidget(help);
    }
    f.error = note(QString(), cell);
    style::set_tone(f.error, style::Tone::Error);
    f.error->setVisible(false);
    v->addWidget(f.error);
    QString label = qs(f.question.prompt);
    if (f.question.type == QuestionType::Bool) label.clear();
    else if (!label.endsWith(QLatin1Char('?')) && !label.endsWith(QLatin1Char(':'))) label += QLatin1Char(':');
    if (f.question.type == QuestionType::Table) {
      // A table takes the page's width, its prompt above it.
      f.form->addRow(new QLabel(label, page));
      f.form->addRow(cell);
      f.label_row = f.form->rowCount() - 2;
    } else {
      f.form->addRow(label, cell);
    }
    if (f.question.default_value) set_value(f, *f.question.default_value);
  }
  // The data-reduction server page can be tried before anything is written.
  if (auto it = field_index_.find("db_host"); it != field_index_.end() && options_.open_database) {
    test_button_ = add_test_row(fields_[it->second].form, test_result_, "test-connection");
    connect(test_button_, &QPushButton::clicked, this, [this] { test_connection(); });
  }
  // So can an instrument: its drivers connect to the address on the page.
  if (profile_->top.kind == ProfileKind::Instrument) {
    const auto g = std::find(groups_.begin(), groups_.end(), std::string("Instrument connection"));
    const auto f = std::find_if(fields_.begin(), fields_.end(), [&](const Field& x) {
      return g != groups_.end() && x.group == static_cast<int>(g - groups_.begin());
    });
    if (f != fields_.end()) {
      instrument_test_button_ = add_test_row(f->form, instrument_test_result_, "test-instrument");
      connect(instrument_test_button_, &QPushButton::clicked, this, [this] { test_instrument(); });
    }
  }
  if (existing_) {
    for (auto& f : fields_) {
      auto it = existing_->answers.find(f.question.id);
      if (it != existing_->answers.end()) set_value(f, it->second);
    }
  }
  building_ = false;
  refresh_visibility();
}

QWidget* SetupWizard::make_editor(Field& field) {
  const Question& q = field.question;
  auto changed = [this] { refresh_visibility(); };
  switch (q.type) {
    case QuestionType::Bool: {
      auto* box = new QCheckBox(qs(q.prompt));
      connect(box, &QCheckBox::toggled, this, changed);
      return box;
    }
    case QuestionType::Port:
    case QuestionType::Int: {
      auto* spin = new QSpinBox;
      if (q.type == QuestionType::Port) spin->setRange(1, 65535);
      else spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
      spin->setMaximumWidth(140);
      connect(spin, &QSpinBox::valueChanged, this, changed);
      return spin;
    }
    case QuestionType::Choice: {
      const bool labelled = q.labels.size() == q.choices.size();
      if (q.choices.size() <= kChoiceRadioLimit) {
        auto* box = new QWidget;
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        auto* group = new QButtonGroup(box);
        for (std::size_t i = 0; i < q.choices.size(); ++i) {
          auto* r = new QRadioButton(qs(labelled ? q.labels[i] : q.choices[i]), box);
          r->setObjectName(qs(q.id + "-" + q.choices[i]));
          group->addButton(r, static_cast<int>(i));
          v->addWidget(r);
        }
        connect(group, &QButtonGroup::idClicked, this, changed);
        return box;
      }
      auto* combo = new QComboBox;
      for (std::size_t i = 0; i < q.choices.size(); ++i) combo->addItem(qs(labelled ? q.labels[i] : q.choices[i]), qs(q.choices[i]));
      connect(combo, &QComboBox::currentIndexChanged, this, changed);
      return combo;
    }
    case QuestionType::Table: {
      auto* box = new QWidget;
      auto* v = new QVBoxLayout(box);
      v->setContentsMargins(0, 0, 0, 0);
      auto* table = new QTableWidget(0, static_cast<int>(q.columns.size()), box);
      table->setObjectName(QStringLiteral("table"));
      QStringList headers;
      for (const auto& c : q.columns) headers << qs(c);
      table->setHorizontalHeaderLabels(headers);
      table->horizontalHeader()->setStretchLastSection(true);
      table->verticalHeader()->setVisible(false);
      table->setMinimumHeight(180);
      auto* buttons = new QHBoxLayout;
      auto* add = new QPushButton(tr("Add row"), box);
      auto* remove = new QPushButton(tr("Remove row"), box);
      buttons->addWidget(add);
      buttons->addWidget(remove);
      buttons->addStretch(1);
      v->addWidget(table);
      v->addLayout(buttons);
      connect(add, &QPushButton::clicked, table, [table] { table->insertRow(table->rowCount()); });
      connect(remove, &QPushButton::clicked, table, [table] {
        const int r = table->currentRow();
        table->removeRow(r >= 0 ? r : table->rowCount() - 1);
      });
      return box;
    }
    case QuestionType::Path:
    case QuestionType::Folder: {
      auto* box = new QWidget;
      auto* h = new QHBoxLayout(box);
      h->setContentsMargins(0, 0, 0, 0);
      auto* edit = new QLineEdit(box);
      edit->setObjectName(QStringLiteral("text"));
      auto* browse = new QPushButton(tr("Browse…"), box);
      h->addWidget(edit, 1);
      h->addWidget(browse);
      connect(edit, &QLineEdit::textChanged, this, changed);
      const bool folder = q.type == QuestionType::Folder;
      connect(browse, &QPushButton::clicked, this, [this, edit, folder] {
        const QString f = folder ? QFileDialog::getExistingDirectory(this, QString(), edit->text())
                                 : QFileDialog::getOpenFileName(this, QString(), edit->text());
        if (!f.isEmpty()) edit->setText(f);
      });
      return box;
    }
    case QuestionType::String:
    case QuestionType::Host:
    case QuestionType::Float:
    case QuestionType::Secret:
    case QuestionType::List: {
      auto* edit = new QLineEdit;
      if (q.type == QuestionType::Secret) edit->setEchoMode(QLineEdit::Password);
      if (q.type == QuestionType::List) edit->setPlaceholderText(tr("comma separated"));
      connect(edit, &QLineEdit::textChanged, this, changed);
      return edit;
    }
  }
  return new QWidget;
}

// --- values -----------------------------------------------------------------

namespace {

QLineEdit* line_edit_of(QWidget* editor) {
  if (auto* e = qobject_cast<QLineEdit*>(editor)) return e;
  return editor->findChild<QLineEdit*>(QStringLiteral("text"));
}

}  // namespace

void SetupWizard::set_value(Field& field, const Value& value) {
  const Question& q = field.question;
  switch (q.type) {
    case QuestionType::Bool:
      if (auto* b = qobject_cast<QCheckBox*>(field.editor)) b->setChecked(truthy(value));
      return;
    case QuestionType::Port:
    case QuestionType::Int:
      if (auto* s = qobject_cast<QSpinBox*>(field.editor)) {
        if (const auto* i = std::get_if<std::int64_t>(&value)) s->setValue(static_cast<int>(*i));
        else s->setValue(QString::fromStdString(to_text(value)).toInt());
      }
      return;
    case QuestionType::Choice: {
      const std::string text = to_text(value);
      const auto it = std::find(q.choices.begin(), q.choices.end(), text);
      if (it == q.choices.end()) return;
      const int index = static_cast<int>(it - q.choices.begin());
      if (auto* combo = qobject_cast<QComboBox*>(field.editor)) {
        combo->setCurrentIndex(index);
      } else if (auto* group = field.editor->findChild<QButtonGroup*>()) {
        if (auto* b = group->button(index)) b->setChecked(true);
      }
      return;
    }
    case QuestionType::Table: {
      auto* table = field.editor->findChild<QTableWidget*>(QStringLiteral("table"));
      const auto* rows = std::get_if<std::vector<Row>>(&value);
      if (table == nullptr || rows == nullptr) return;
      table->setRowCount(static_cast<int>(rows->size()));
      for (std::size_t r = 0; r < rows->size(); ++r) {
        for (std::size_t c = 0; c < q.columns.size(); ++c) {
          auto cell = (*rows)[r].find(q.columns[c]);
          table->setItem(static_cast<int>(r), static_cast<int>(c),
                         new QTableWidgetItem(cell == (*rows)[r].end() ? QString() : qs(cell->second)));
        }
      }
      return;
    }
    default:
      if (auto* e = line_edit_of(field.editor)) {
        if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
          QStringList parts;
          for (const auto& s : *list) parts << qs(s);
          e->setText(parts.join(QStringLiteral(", ")));
        } else {
          e->setText(qs(to_text(value)));
        }
      }
      return;
  }
}

Result<Value> SetupWizard::value_of(const Field& field) const {
  const Question& q = field.question;
  switch (q.type) {
    case QuestionType::Bool:
      return Value{qobject_cast<QCheckBox*>(field.editor)->isChecked()};
    case QuestionType::Port:
    case QuestionType::Int:
      return Value{static_cast<std::int64_t>(qobject_cast<QSpinBox*>(field.editor)->value())};
    case QuestionType::Choice: {
      int index = -1;
      if (auto* combo = qobject_cast<QComboBox*>(field.editor)) index = combo->currentIndex();
      else if (auto* group = field.editor->findChild<QButtonGroup*>()) index = group->checkedId();
      if (index < 0 || index >= static_cast<int>(q.choices.size())) return fail(ErrorKind::Config, "choose one");
      return Value{q.choices[static_cast<std::size_t>(index)]};
    }
    case QuestionType::Table: {
      auto* table = field.editor->findChild<QTableWidget*>(QStringLiteral("table"));
      std::vector<Row> rows;
      for (int r = 0; r < table->rowCount(); ++r) {
        Row row;
        bool any = false;
        for (int c = 0; c < table->columnCount(); ++c) {
          const QTableWidgetItem* item = table->item(r, c);
          const std::string text = item ? ss(item->text().trimmed()) : std::string{};
          any = any || !text.empty();
          row[q.columns[static_cast<std::size_t>(c)]] = text;
        }
        if (any) rows.push_back(std::move(row));
      }
      return Value{std::move(rows)};
    }
    default: {
      const QLineEdit* e = line_edit_of(field.editor);
      const QString text = e ? e->text().trimmed() : QString();
      if (q.type == QuestionType::Secret) return Value{ss(e ? e->text() : QString())};
      auto v = parse_answer(q, ss(text));
      if (!v) {
        // parse_answer prefixes the question id; the field shows it alone.
        std::string what = v.error().what;
        if (what.starts_with(q.id + ": ")) what = what.substr(q.id.size() + 2);
        return fail(ErrorKind::Config, what);
      }
      return v;
    }
  }
}

Answers SetupWizard::so_far() const {
  Answers out = builtin_answers(ss(name_->text().trimmed()), root());
  if (profile_) {
    for (const auto& [k, v] : profile_->values) out[k] = v;
  }
  for (const auto& f : fields_) {
    if (auto v = value_of(f)) out[f.question.id] = std::move(*v);
  }
  return out;
}

Result<Answers> SetupWizard::answers() const {
  Answers out;
  const Answers context = so_far();
  std::string errors;
  for (const auto& f : fields_) {
    if (!is_asked(f.question, context)) continue;
    auto v = value_of(f);
    if (!v) {
      errors += (errors.empty() ? "" : "\n") + f.question.id + ": " + v.error().what;
      continue;
    }
    out[f.question.id] = std::move(*v);
  }
  if (!errors.empty()) return fail(ErrorKind::Config, errors);
  return out;
}

void SetupWizard::refresh_visibility() {
  if (building_) return;
  const Answers context = so_far();
  for (auto& f : fields_) {
    const bool shown = is_asked(f.question, context);
    f.form->setRowVisible(f.editor->parentWidget(), shown);
    if (f.label_row >= 0) f.form->setRowVisible(f.label_row, shown);
  }
  if (test_button_ != nullptr) {
    const auto it = context.find("data_source");
    const bool server = it != context.end() && to_text(it->second) == "server";
    test_button_->parentWidget()->setVisible(server);
  }
}

bool SetupWizard::group_has_questions(int group) const {
  const Answers context = so_far();
  return std::any_of(fields_.begin(), fields_.end(),
                     [&](const Field& f) { return f.group == group && is_asked(f.question, context); });
}

// --- navigation -------------------------------------------------------------

int SetupWizard::nextId() const {
  const int id = currentId();
  if (id == kDone) return -1;
  if (id == kReady) return kDone;
  const int from = id == kWelcome ? -1 : id == kLocation ? 0 : id - kFirstGroup + 1;
  if (id == kWelcome) return kLocation;
  for (int g = std::max(from, 0); g < static_cast<int>(groups_.size()); ++g) {
    if (group_has_questions(g)) return kFirstGroup + g;
  }
  return kReady;
}

bool SetupWizard::validateCurrentPage() {
  const int id = currentId();
  if (id == kWelcome) {
    const QString name = chosen();
    if (name.isEmpty()) return false;
    if (!profile_ || profile_->top.name != ss(name)) {
      auto resolved = library_.resolve(ss(name));
      if (!resolved) {
        location_note_->setText(qs(resolved.error().what));
        return false;
      }
      profile_ = std::move(*resolved);
      existing_.reset();
      if (!name_edited_) name_->setText(qs(default_install_name(profile_->top)));
      if (!root_edited_) root_->setText(qs(default_root(profile_->top, ss(name_->text())).string()));
      rebuild_groups();
    }
    return true;
  }
  if (id == kLocation) return validate_location();
  if (id >= kFirstGroup && id < kReady) return validate_group(id - kFirstGroup);
  if (id == kReady) return install();
  return QWizard::validateCurrentPage();
}

bool SetupWizard::validate_location() {
  const QString name = name_->text().trimmed();
  if (name.isEmpty() || root_->text().trimmed().isEmpty()) {
    location_note_->setText(tr("Give the installation a name and a folder."));
    return false;
  }
  const fs::path dir = root();
  std::error_code ec;
  auto record = read_install_record(dir);
  const bool had = existing_.has_value();
  existing_.reset();
  if (record) {
    if (record->profile != profile_->top.name) {
      location_note_->setText(tr("%1 already holds a %2 installation; choose another folder.")
                                  .arg(qs(dir.string()), qs(record->profile)));
      return false;
    }
    existing_ = *record;
    location_note_->setText(tr("This folder already holds this installation: its settings are filled in, "
                               "and files you have edited are kept."));
    for (auto& f : fields_) {
      auto it = existing_->answers.find(f.question.id);
      if (it != existing_->answers.end()) set_value(f, it->second);
    }
    refresh_visibility();
  } else if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) {
    location_note_->setText(tr("The folder is not empty. Files already there are kept as they are."));
  } else {
    location_note_->setText(QString());
  }
  // Leaving an existing install's folder: back to the defaults.
  if (had && !existing_) {
    for (auto& f : fields_) {
      if (f.question.default_value) set_value(f, *f.question.default_value);
    }
    refresh_visibility();
  }
  return true;
}

bool SetupWizard::validate_group(int group) {
  const Answers context = so_far();
  bool ok = true;
  for (auto& f : fields_) {
    if (f.group != group) continue;
    f.error->setVisible(false);
    if (!is_asked(f.question, context)) continue;
    auto v = value_of(f);
    if (!v) {
      f.error->setText(qs(v.error().what));
      f.error->setVisible(true);
      ok = false;
    }
  }
  return ok;
}

void SetupWizard::initializePage(int id) {
  if (id == kReady) prepare_ready();
  if (id == kDone) {
    QString html;
    const bool failed = any_fail(checks_);
    page(kDone)->setTitle(failed ? tr("Installed, with problems to fix") : tr("Pychron is set up"));
    html += QStringLiteral("<p>%1 <b>%2</b> is in <code>%3</code>.</p>")
                .arg(tr("The installation"), escaped(installed_ ? installed_->name : ""),
                     escaped(installed_ ? installed_->root.string() : ""));
    html += QStringLiteral("<table cellspacing=4>");
    for (const auto& c : checks_) {
      const auto& t = theme();
      const QString mark = c.status == Check::Status::Ok
                               ? QStringLiteral("<span style='color:%1'>✓</span>").arg(t.ok.name())
                           : c.status == Check::Status::Warn
                               ? QStringLiteral("<span style='color:%1'>!</span>").arg(t.warning_text.name())
                               : QStringLiteral("<span style='color:%1'>✗</span>").arg(t.error_text.name());
      html += QStringLiteral("<tr><td>%1</td><td><b>%2</b></td><td>%3%4</td></tr>")
                  .arg(mark, escaped(c.name), escaped(c.detail),
                       c.hint.empty() ? QString() : QStringLiteral("<br><i>%1</i>").arg(escaped(c.hint)));
    }
    html += QStringLiteral("</table>");
    if (plan_ && !plan_->placeholders.empty()) {
      html += QStringLiteral("<p>%1</p><ul>").arg(tr("Before measuring, replace the placeholders in these files "
                                                      "with your instrument's values (see CALIBRATE.md):"));
      for (const auto& p : plan_->placeholders) html += QStringLiteral("<li><code>%1</code></li>").arg(escaped(p.generic_string()));
      html += QStringLiteral("</ul>");
    }
    done_report_->setHtml(html);
  }
  QWizard::initializePage(id);
}

// --- install ----------------------------------------------------------------

fs::path SetupWizard::root() const { return fs::path(ss(root_->text().trimmed())); }

void SetupWizard::prepare_ready() {
  auto* page = static_cast<ReadyPage*>(this->page(kReady));
  plan_.reset();
  ready_error_->setVisible(false);
  QString html;
  auto fail_with = [&](const std::string& what) {
    ready_error_->setText(qs(what));
    ready_error_->setVisible(true);
    summary_->setHtml(html);
    page->changed();
  };
  if (!profile_) return fail_with("choose what to set up");
  auto given = answers();
  if (!given) return fail_with(given.error().what);
  const bool reconfigure = existing_.has_value();
  // An existing install whose secret was left blank keeps the file holding it.
  bool keep_secrets = false;
  if (reconfigure) {
    for (const auto& q : profile_->questions) {
      auto it = given->find(q.id);
      if (q.type == QuestionType::Secret && it != given->end() && to_text(it->second).empty()) keep_secrets = true;
    }
  }
  const std::string name = ss(name_->text().trimmed());
  auto answers = complete_answers(*profile_, *given, builtin_answers(name, root()));
  if (!answers) return fail_with(answers.error().what);
  auto plan = plan_install(library_, *profile_, *answers, root(), PlanOptions{reconfigure, keep_secrets});
  if (!plan) return fail_with(plan.error().what);

  html += QStringLiteral("<p><b>%1</b> — %2</p>").arg(escaped(profile_->top.title), escaped(profile_->top.summary));
  html += QStringLiteral("<p>%1 <b>%2</b><br>%3 <code>%4</code></p>")
              .arg(tr("Name:"), escaped(name), tr("Folder:"), escaped(root().string()));
  html += QStringLiteral("<table cellspacing=2>");
  for (std::size_t g = 0; g < groups_.size(); ++g) {
    bool heading = false;
    for (const auto& f : fields_) {
      if (f.group != static_cast<int>(g) || !is_asked(f.question, *answers)) continue;
      auto it = answers->find(f.question.id);
      if (it == answers->end()) continue;
      if (!heading) {
        heading = true;
        html += QStringLiteral("<tr><td colspan=2><b>%1</b></td></tr>").arg(escaped(groups_[g]));
      }
      QString value;
      if (f.question.type == QuestionType::Bool) value = truthy(it->second) ? tr("Yes") : tr("No");
      else if (f.question.type == QuestionType::Secret) value = to_text(it->second).empty() ? tr("(unchanged)") : QStringLiteral("••••");
      else if (const auto* rows = std::get_if<std::vector<Row>>(&it->second)) value = tr("%n row(s)", nullptr, static_cast<int>(rows->size()));
      else if (f.question.type == QuestionType::Choice && f.question.labels.size() == f.question.choices.size()) {
        const auto c = std::find(f.question.choices.begin(), f.question.choices.end(), to_text(it->second));
        value = c == f.question.choices.end() ? qs(to_text(it->second))
                                              : qs(f.question.labels[static_cast<std::size_t>(c - f.question.choices.begin())]);
      } else value = qs(to_text(it->second));
      html += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>").arg(escaped(f.question.prompt), value.toHtmlEscaped());
    }
  }
  html += QStringLiteral("</table>");
  int write = 0, keep = 0, update = 0, conflict = 0;
  for (const auto& f : plan->files) {
    switch (f.action) {
      case PlannedFile::Action::Write: ++write; break;
      case PlannedFile::Action::Keep: ++keep; break;
      case PlannedFile::Action::Update: ++update; break;
      case PlannedFile::Action::Conflict: ++conflict; break;
      case PlannedFile::Action::Same: break;
    }
  }
  html += QStringLiteral("<p>%1").arg(tr("%n file(s) to write", nullptr, write));
  if (update > 0) html += QStringLiteral(", %1").arg(tr("%n to update", nullptr, update));
  if (keep > 0) html += QStringLiteral(", %1").arg(tr("%n already there and kept", nullptr, keep));
  if (conflict > 0) html += QStringLiteral(", %1").arg(tr("%n you edited (the new version goes beside it as .new)", nullptr, conflict));
  html += QStringLiteral(".</p>");
  if (profile_->top.kind == ProfileKind::DataReduction) {
    const std::string url = database_url_for(*answers, root(), false);
    html += QStringLiteral("<p>%1 <code>%2</code></p>").arg(tr("Database:"), escaped(url));
  }
  if (!plan->notes.empty()) {
    html += QStringLiteral("<p>%1</p><ul>").arg(tr("Converting the legacy setup: review these before running on hardware."));
    for (const auto& n : plan->notes) html += QStringLiteral("<li>%1</li>").arg(escaped(n));
    html += QStringLiteral("</ul>");
  }
  summary_->setHtml(html);
  plan_ = std::move(*plan);
  page->changed();
}

bool SetupWizard::install() {
  if (!plan_) return false;
  auto show_error = [this](const std::string& what) {
    ready_error_->setText(qs(what));
    ready_error_->setVisible(true);
    return false;
  };
  QApplication::setOverrideCursor(Qt::WaitCursor);
  struct Restore {
    ~Restore() { QApplication::restoreOverrideCursor(); }
  } restore;
  auto report = apply_install(*plan_);
  if (!report) return show_error(report.error().what);
  SiteInstall entry = site_install(*plan_, ss(name_->text().trimmed()));
  if (entry.kind == "data_reduction" && entry.database.starts_with("sqlite:") && options_.open_database) {
    // A local database: create it and bring its schema up to date.
    if (auto made = options_.open_database(entry.database, true); !made) {
      return show_error("creating " + entry.database + ": " + made.error().what);
    }
  }
  // An instrument's database is made (a local one; the lab's server is never
  // migrated from here) and given the install's seed file. The instrument
  // measures without it: what goes wrong is a warning on the last page.
  std::optional<Check> seeded;
  const std::filesystem::path seed_file = entry.root / "seed.toml";
  std::error_code ec;
  if (entry.kind == "instrument" && !entry.database.empty() && options_.seed_database &&
      std::filesystem::exists(seed_file, ec)) {
    const bool local = entry.database.starts_with("sqlite:");
    Result<std::string> done = database_url(entry);
    if (done) {
      // SQLite makes the file, not the folder it is in.
      if (local)
        std::filesystem::create_directories(
            std::filesystem::path(done->substr(std::string_view("sqlite:").size())).parent_path(), ec);
      done = options_.seed_database(*done, seed_file, local);
    }
    seeded = done ? Check{Check::Status::Ok, "seed", *done, {}}
                  // A server's password is in the install, not in its url:
                  // there the install is the one to run again.
                  : Check{Check::Status::Warn, "seed",
                          "skipped: " + done.error().what.substr(0, done.error().what.find('\n')),
                          local ? "elctl entry seed \"" + seed_file.string() + "\" --db \"" + entry.database + "\""
                                : "elctl init --reconfigure --install " + entry.name};
  }
  if (auto saved = register_install(entry, options_.site_path); !saved) return show_error(saved.error().what);
  installed_ = entry;
  DoctorOptions doctor_options;
  doctor_options.library = &library_;
  if (options_.open_database) {
    doctor_options.open_database = [open = options_.open_database](const std::string& url) { return open(url, false); };
  }
  checks_ = doctor(entry, doctor_options);
  if (seeded) checks_.push_back(std::move(*seeded));
  return true;
}

QPushButton* SetupWizard::add_test_row(QFormLayout* form, QLabel*& result, const char* name) {
  auto* row = new QWidget(form->parentWidget());
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  auto* button = new QPushButton(tr("Test connection"), row);
  button->setObjectName(QString::fromLatin1(name));
  result = note(QString(), row);
  h->addWidget(button);
  h->addWidget(result, 1);
  form->addRow(QString(), row);
  return button;
}

void SetupWizard::test_instrument() {
  auto show = [this](bool ok, std::string what) {
    if (const auto nl = what.find('\n'); nl != std::string::npos) what.resize(nl);
    style::set_tone(instrument_test_result_, ok ? style::Tone::Accent : style::Tone::Error);
    instrument_test_result_->setText(qs(what));
  };
  auto given = answers();
  if (!given) return show(false, given.error().what);
  const std::string name = ss(name_->text().trimmed());
  auto complete = complete_answers(*profile_, *given, builtin_answers(name.empty() ? "test" : name, root()));
  if (!complete) return show(false, complete.error().what);
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto connected = options_.test_instrument ? options_.test_instrument(library_, *profile_, *complete)
                                            : test_instrument_connection(library_, *profile_, *complete);
  QApplication::restoreOverrideCursor();
  if (connected) show(true, *connected);
  else show(false, connected.error().what);
}

void SetupWizard::test_connection() {
  auto given = answers();
  if (!given) {
    test_result_->setText(qs(given.error().what));
    return;
  }
  const std::string url = database_url_for(*given, root(), true);
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto opened = options_.open_database(url, false);
  QApplication::restoreOverrideCursor();
  if (opened) {
    style::set_tone(test_result_, style::Tone::Accent);
    test_result_->setText(tr("Connected (%1)").arg(qs(*opened)));
  } else {
    style::set_tone(test_result_, style::Tone::Error);
    std::string what = opened.error().what;
    if (const auto nl = what.find('\n'); nl != std::string::npos) what.resize(nl);
    test_result_->setText(qs(what));
  }
}

// --- accessors --------------------------------------------------------------

bool SetupWizard::open_now() const { return installed_.has_value() && open_now_->isChecked(); }

void SetupWizard::choose(const QString& profile) {
  for (std::size_t i = 0; i < choice_names_.size(); ++i) {
    if (choice_names_[i] == ss(profile)) choices_->button(static_cast<int>(i))->setChecked(true);
  }
}

QString SetupWizard::chosen() const {
  const int id = choices_->checkedId();
  return id < 0 ? QString() : qs(choice_names_[static_cast<std::size_t>(id)]);
}

QWidget* SetupWizard::editor(const QString& id) const {
  auto it = field_index_.find(ss(id));
  return it == field_index_.end() ? nullptr : fields_[it->second].editor;
}

bool SetupWizard::is_shown(const QString& id) const {
  auto it = field_index_.find(ss(id));
  return it != field_index_.end() && is_asked(fields_[it->second].question, so_far());
}

QString SetupWizard::error_for(const QString& id) const {
  auto it = field_index_.find(ss(id));
  if (it == field_index_.end() || !fields_[it->second].error->isVisibleTo(fields_[it->second].error->parentWidget()))
    return {};
  return fields_[it->second].error->text();
}

}  // namespace pychron::ui
