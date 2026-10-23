#include "data_source_page.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include <QApplication>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include "pychron/setup/doctor.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/template.hpp"
#include "error_report.hpp"
#include "question_editor.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace fs = std::filesystem;
using namespace pychron::setup;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// The store-common questions the page asks, in this order.
constexpr std::array<std::string_view, 8> kIds{"data_source", "db_host", "db_port",  "db_name",
                                               "db_user",     "db_password", "db_file", "data_folder"};

// `url` with the password of its user part left out: what is said on the page.
std::string without_password(std::string url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  const auto start = scheme + 3;
  const auto at = url.find('@', start);
  if (at == std::string::npos || url.find('/', start) < at) return url;
  const auto colon = url.find(':', start);
  if (colon != std::string::npos && colon < at) url.erase(colon, at - colon);
  return url;
}

QLabel* wrapped(QWidget* parent) {
  auto* l = new QLabel(parent);
  l->setWordWrap(true);
  l->setTextFormat(Qt::PlainText);
  l->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return l;
}

}  // namespace

DataSourcePage::DataSourcePage(DataSourcePageDeps deps, QWidget* parent) : QWidget(parent), deps_(std::move(deps)) {
  auto* layout = new QVBoxLayout(this);
  form_ = new QFormLayout;
  form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->addLayout(form_);

  for (const std::string_view id : kIds) {
    const auto q = std::find_if(deps_.questions.begin(), deps_.questions.end(),
                                [id](const Question& x) { return x.id == id; });
    if (q == deps_.questions.end()) continue;
    Field f;
    f.question = *q;
    f.cell = new QWidget(this);
    auto* row = new QHBoxLayout(f.cell);
    row->setContentsMargins(0, 0, 0, 0);
    f.editor = make_question_editor(f.question, f.cell, [this] {
      if (!building_) edited_ = true;  // by the user: what the page shows itself is not an edit
      refresh_visibility();
    });
    f.editor->setObjectName(qs("answer-" + f.question.id));
    row->addWidget(f.editor, 1);
    if (id == "db_file") {
      // A database that is not there yet: made and migrated when applied.
      auto* make = new QPushButton(tr("New file…"), f.cell);
      make->setObjectName(QStringLiteral("new-file"));
      row->addWidget(make);
      QWidget* editor = f.editor;
      connect(make, &QPushButton::clicked, this, [this, editor] {
        auto* edit = editor->findChild<QLineEdit*>(QStringLiteral("text"));
        if (edit == nullptr) return;
        const QString file = QFileDialog::getSaveFileName(this, tr("New database file"), edit->text(),
                                                          tr("SQLite database (*.db);;All files (*)"));
        if (!file.isEmpty()) edit->setText(file);
      });
    }
    QString label = qs(f.question.prompt);
    if (!label.endsWith(QLatin1Char('?')) && !label.endsWith(QLatin1Char(':'))) label += QLatin1Char(':');
    form_->addRow(label, f.cell);
    if (f.question.default_value) set_question_value(f.editor, f.question, *f.question.default_value);
    fields_.push_back(std::move(f));
  }

  auto* tests = new QHBoxLayout;
  test_button_ = new QPushButton(tr("Test connection"), this);
  test_button_->setObjectName(QStringLiteral("test-connection"));
  tests->addWidget(test_button_);
  tests->addStretch(1);
  layout->addLayout(tests);
  connect(test_button_, &QPushButton::clicked, this, [this] { test_connection(); });
  result_ = wrapped(this);
  result_->setObjectName(QStringLiteral("data-source-result"));
  layout->addWidget(result_);
  note_ = wrapped(this);
  note_->setObjectName(QStringLiteral("data-source-note"));
  style::set_tone(note_, style::Tone::Muted);
  layout->addWidget(note_);
  layout->addStretch(1);

  auto current = current_data_source(deps_.install);
  if (current) {
    current_ = *current;
    show_source(*current);
  } else {
    // Nothing on the page stands for the install then: it is shown, and never applied.
    unreadable_ = "the install's data source could not be read: " + current.error().what;
    say({unreadable_}, false);
  }
  show_kept_password();
  building_ = false;
  refresh_visibility();
  refresh_read_only();
}

const DataSourcePage::Field* DataSourcePage::field(const std::string& id) const {
  const auto f = std::find_if(fields_.begin(), fields_.end(), [&](const Field& x) { return x.question.id == id; });
  return f == fields_.end() ? nullptr : &*f;
}

QWidget* DataSourcePage::editor(const QString& id) const {
  const Field* f = field(id.toStdString());
  return f ? f->editor : nullptr;
}

void DataSourcePage::show_source(const DataSource& source) {
  auto set = [this](const char* id, const Value& v) {
    if (const Field* f = field(id)) set_question_value(f->editor, f->question, v);
  };
  if (const auto* local = std::get_if<LocalDatabase>(&source.database)) {
    set("data_source", Value{std::string("local")});
    set("db_file", Value{local->file.generic_string()});
  } else {
    const auto& server = std::get<ServerDatabase>(source.database);
    set("data_source", Value{std::string("server")});
    set("db_host", Value{server.host});
    set("db_port", Value{std::int64_t{server.port}});
    set("db_name", Value{server.name});
    set("db_user", Value{server.user});
  }
  set("data_folder", Value{source.data});
}

// Blank keeps the stored password, and says so when there is one.
void DataSourcePage::show_kept_password() {
  const Field* f = field("db_password");
  auto* edit = f ? qobject_cast<QLineEdit*>(f->editor) : nullptr;
  if (edit == nullptr) return;
  edit->clear();
  auto kept = read_kept_password(deps_.install.root);
  edit->setPlaceholderText(kept && *kept ? tr("kept") : QString());
}

Answers DataSourcePage::so_far() const {
  Answers out;
  for (const auto& f : fields_) {
    if (auto v = question_value(f.editor, f.question)) out[f.question.id] = std::move(*v);
  }
  return out;
}

void DataSourcePage::refresh_visibility() {
  if (building_) return;
  const Answers context = so_far();
  for (const auto& f : fields_) form_->setRowVisible(f.cell, is_asked(f.question, context));
}

Result<DataSource> DataSourcePage::source() const {
  auto value = [this](const char* id) -> Result<Value> {
    const Field* f = field(id);
    if (f == nullptr) return fail(ErrorKind::Config, std::string("no field ") + id);
    auto v = question_value(f->editor, f->question);
    if (!v) return fail(ErrorKind::Config, f->question.prompt + ": " + v.error().what);
    return v;
  };
  auto text = [&](const char* id) -> Result<std::string> {
    auto v = value(id);
    if (!v) return fail(std::move(v).error());
    return to_text(*v);
  };

  DataSource out;
  auto kind = text("data_source");
  if (!kind) return fail(std::move(kind).error());
  if (*kind == "server") {
    ServerDatabase server;
    auto host = text("db_host");
    if (!host) return fail(std::move(host).error());
    auto port = value("db_port");
    if (!port) return fail(std::move(port).error());
    auto name = text("db_name");
    if (!name) return fail(std::move(name).error());
    auto user = text("db_user");
    if (!user) return fail(std::move(user).error());
    server.host = *host;
    if (const auto* n = std::get_if<std::int64_t>(&*port)) server.port = static_cast<int>(*n);
    server.name = *name;
    server.user = *user;
    if (const Field* f = field("db_password")) {
      const auto* edit = qobject_cast<const QLineEdit*>(f->editor);
      if (edit != nullptr && !edit->text().isEmpty()) server.password = edit->text().toStdString();
    }
    out.database = std::move(server);
  } else {
    auto file = text("db_file");
    if (!file) return fail(std::move(file).error());
    out.database = LocalDatabase{fs::path(*file)};
  }
  auto folder = text("data_folder");
  if (!folder) return fail(std::move(folder).error());
  out.data = *folder;
  return out;
}

bool DataSourcePage::changed() const {
  if (!current_) return false;
  // A field that is no answer (a recorded port of 70000) is a change only once
  // the user has edited the page: until then it blocks no other page.
  auto now = source();
  if (!now) return edited_;
  return *now != *current_;
}

void DataSourcePage::say(const std::vector<std::string>& lines, bool ok) {
  QStringList text;
  for (const auto& l : lines) text << qs(l);
  if (!ok) log_shown("preferences", tr("Data source"), text.join(QStringLiteral("; ")));
  style::set_tone(result_, ok ? style::Tone::Accent : style::Tone::Error);
  result_->setText(text.join(QLatin1Char('\n')));
}

void DataSourcePage::test_connection() {
  auto to = source();
  if (!to) return say({to.error().what}, false);
  auto change = plan_data_source(deps_.install, *to);
  if (!change) return say({change.error().what}, false);
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto check = check_data_source(*change, *to, deps_.openers, false);
  QApplication::restoreOverrideCursor();
  if (!check) return say({check.error().what}, false);
  say(check->said, true);
}

QString DataSourcePage::apply(bool check) {
  if (!current_) return qs(unreadable_);
  if (!changed()) return {};
  if (deps_.queue_running && deps_.queue_running()) return tr("Stop the queue to change the data source");
  auto to = source();
  if (!to) return qs(to.error().what);
  auto change = plan_data_source(deps_.install, *to);
  if (!change) return qs(change.error().what);

  DataSourceCheck checked;  // without the checks: nothing opened, nothing seeded
  std::vector<std::string> said;
  if (check) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto result = check_data_source(*change, *to, deps_.openers, true);
    QApplication::restoreOverrideCursor();
    if (!result) {
      say({result.error().what}, false);
      return qs(result.error().what);
    }
    checked = std::move(*result);
    said = checked.said;
  } else {
    said.emplace_back("not checked");
  }
  if (auto applied = apply_data_source(*change, *to, deps_.site_path); !applied) {
    say({applied.error().what}, false);
    return qs(applied.error().what);
  }
  said.push_back("written: install '" + change->install.name + "' in " + deps_.site_path.string());
  for (auto& line : seed_data_source(*change, *to, deps_.openers, checked)) said.push_back(std::move(line));
  for (const auto& note : change->notes) said.push_back(note);
  say(said, true);

  deps_.install = change->install;
  DataSource now = *to;
  if (auto* server = std::get_if<ServerDatabase>(&now.database)) server->password.reset();
  current_ = std::move(now);
  building_ = true;
  show_kept_password();
  building_ = false;
  edited_ = false;
  return {};
}

void DataSourcePage::showEvent(QShowEvent* event) {
  refresh_read_only();
  QWidget::showEvent(event);
}

void DataSourcePage::refresh_read_only() {
  const bool running = deps_.queue_running && deps_.queue_running();
  const bool locked = running || !current_;
  for (const auto& f : fields_) f.cell->setEnabled(!locked);
  test_button_->setEnabled(!locked);
  QStringList notes;
  if (running) notes << tr("Stop the queue to change the data source");
  if (deps_.session_database) {
    notes << tr("This session uses --db %1; changes here are for the install")
                 .arg(qs(without_password(*deps_.session_database)));
  }
  note_->setText(notes.join(QLatin1Char('\n')));
  note_->setVisible(!notes.isEmpty());
}

}  // namespace pychron::ui
