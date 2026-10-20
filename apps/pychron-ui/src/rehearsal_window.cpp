#include "rehearsal_window.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "canvas_view.hpp"
#include "code_editor.hpp"
#include "pychron/experiment/lab/scripts.hpp"
#include "script_highlighter.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

using experiment::rehearsal::LineSnapshot;
using experiment::rehearsal::StepRecord;
using experiment::rehearsal::Waiting;

// The fields a script reads most, shown first; the rest go under More.
constexpr std::array<const char*, 5> kFirstFields{"analysis_type", "run_identifier", "extract_value", "extract_units",
                                                  "duration"};

constexpr int kIndexColumn = 0;
constexpr int kWhereColumn = 1;
constexpr int kCallColumn = 2;
constexpr int kOutcomeColumn = 3;

QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString number_text(double v) { return QString::number(v, 'g', 12); }

// A value as the script wrote it.
QString repr(const scripting::Value& v) {
  if (std::holds_alternative<std::monostate>(v)) return QStringLiteral("None");
  if (const auto* b = std::get_if<bool>(&v)) return *b ? QStringLiteral("True") : QStringLiteral("False");
  if (const auto* i = std::get_if<std::int64_t>(&v)) return QString::number(*i);
  if (const auto* d = std::get_if<double>(&v)) return number_text(*d);
  return QLatin1Char('\'') + q(std::get<std::string>(v)) + QLatin1Char('\'');
}

// "open('P2')", "sleep(2)", "extract(5, units='watts')".
QString call_text(const scripting::StepCall& call) {
  QStringList args;
  for (const auto& a : call.args) args << repr(a);
  for (const auto& [k, v] : call.kwargs) args << q(k) + QLatin1Char('=') + repr(v);
  return q(call.command) + QLatin1Char('(') + args.join(QStringLiteral(", ")) + QLatin1Char(')');
}

// A field's text, read as its default's type.
scripting::Value read_text(const QString& text, const scripting::Value& kind) {
  const QString t = text.trimmed();
  bool ok = false;
  if (std::holds_alternative<std::string>(kind)) return text.toStdString();
  if (std::holds_alternative<std::int64_t>(kind)) {
    const qlonglong i = t.toLongLong(&ok);
    if (ok) return static_cast<std::int64_t>(i);
  }
  if (std::holds_alternative<double>(kind) || std::holds_alternative<std::int64_t>(kind) ||
      std::holds_alternative<std::monostate>(kind)) {
    if (t.isEmpty() && std::holds_alternative<std::monostate>(kind)) return std::monostate{};
    const double d = t.toDouble(&ok);
    if (ok) return d;
  }
  return text.toStdString();  // not a number: the script sees what was typed
}

QString field_text(const scripting::Value& v) {
  if (std::holds_alternative<std::monostate>(v)) return {};
  if (const auto* s = std::get_if<std::string>(&v)) return q(*s);
  return repr(v);
}

}  // namespace

// --- RehearsalCanvasState ---------------------------------------------------

RehearsalCanvasState::RehearsalCanvasState(const CoreBridge& bridge, QObject* parent)
    : ICanvasState(parent), bridge_(bridge), state_(bridge.state()) {
  state_.pending.clear();
}

void RehearsalCanvasState::reset() {
  state_ = bridge_.state();
  state_.pending.clear();
  emit changed();
}

void RehearsalCanvasState::apply(const LineSnapshot& copy) {
  for (const auto& [name, view] : copy.switches) {
    state_.valves[name] = view.state;
    systems::SwitchInfo& info = state_.switches[name];
    info.name = name;
    info.state = view.state;
    info.locked = view.locked;
    info.owner = view.owner;
  }
  emit changed();
}

bool RehearsalCanvasState::gauge_defined(const std::string& name) const {
  const auto& defined = bridge_.config().gauges;
  return std::any_of(defined.begin(), defined.end(), [&](const auto& d) { return d.name == name; });
}

LineSnapshot rehearsal_snapshot(const CoreBridge::State& state,
                                std::map<std::string, systems::InterlockSet, std::less<>> interlocks) {
  LineSnapshot out;
  for (const auto& [name, info] : state.switches) {
    const auto valve = state.valves.find(name);
    out.switches[name] =
        systems::SwitchView{name, valve != state.valves.end() ? valve->second : info.state, info.locked, info.owner};
  }
  for (const auto& [name, valve_state] : state.valves)
    if (!out.switches.contains(name)) out.switches[name] = systems::SwitchView{name, valve_state, false, {}};
  out.interlocks = std::move(interlocks);
  for (const auto& [gauge, value] : state.pressures) out.pressures[gauge] = value;
  return out;
}

// --- RehearsalWindow --------------------------------------------------------

RehearsalWindow::RehearsalWindow(scripting::IScriptHost& host, const CoreBridge& bridge,
                                 std::map<std::string, systems::InterlockSet, std::less<>> interlocks,
                                 const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings, QWidget* parent)
    : QMainWindow(parent),
      host_(host),
      bridge_(bridge),
      interlocks_(std::move(interlocks)),
      lab_(lab),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      canvas_state_(bridge),
      fields_(new QFormLayout),
      more_fields_(new QFormLayout),
      canvas_(new CanvasView(canvas_state_, nullptr)),
      stack_(new QLabel),
      source_(new CodeEditor),
      log_(new QTableWidget(0, 4)),
      status_(new QLabel(tr("Rehearsal: no valve is actuated"))),
      start_(new QAction(tr("Start"), this)),
      step_(new QAction(tr("Step"), this)),
      run_to_end_(new QAction(tr("Run to End"), this)),
      stop_(new QAction(tr("Stop"), this)),
      restart_(new QAction(tr("Restart"), this)) {
  setObjectName(QStringLiteral("RehearsalWindow"));
  setWindowTitle(tr("Rehearse Script"));
  resize(1200, 820);

  // Top: the run's fields.
  auto* form_box = new QWidget;
  auto* form_column = new QVBoxLayout(form_box);
  form_column->setContentsMargins(8, 6, 8, 2);
  form_column->addLayout(fields_);
  auto* more_toggle = new QToolButton;
  more_toggle->setText(tr("More fields"));
  more_toggle->setCheckable(true);
  more_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  more_toggle->setArrowType(Qt::RightArrow);
  auto* more_box = new QWidget;
  more_box->setLayout(more_fields_);
  more_box->setVisible(false);
  connect(more_toggle, &QToolButton::toggled, this, [more_toggle, more_box](bool on) {
    more_box->setVisible(on);
    more_toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
  });
  form_column->addWidget(more_toggle);
  form_column->addWidget(more_box);
  build_fields();

  // Right: the source, read only, with the gosubs that led to it above.
  source_->setReadOnly(true);
  source_->setObjectName(QStringLiteral("RehearsalSource"));
  stack_->setVisible(false);
  style::set_tone(stack_, style::Tone::Muted);
  auto* source_box = new QWidget;
  auto* source_column = new QVBoxLayout(source_box);
  source_column->setContentsMargins(0, 0, 0, 0);
  source_column->addWidget(stack_);
  source_column->addWidget(source_, 1);

  auto* middle = new QSplitter(Qt::Horizontal);
  middle->addWidget(canvas_);
  middle->addWidget(source_box);
  middle->setStretchFactor(0, 3);
  middle->setStretchFactor(1, 2);

  // Bottom: the step log.
  log_->setObjectName(QStringLiteral("RehearsalLog"));
  log_->setHorizontalHeaderLabels({tr("#"), tr("Where"), tr("Call"), tr("Outcome")});
  log_->verticalHeader()->setVisible(false);
  log_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  log_->setSelectionBehavior(QAbstractItemView::SelectRows);
  log_->horizontalHeader()->setSectionResizeMode(kIndexColumn, QHeaderView::ResizeToContents);
  log_->horizontalHeader()->setSectionResizeMode(kWhereColumn, QHeaderView::ResizeToContents);
  log_->horizontalHeader()->setSectionResizeMode(kCallColumn, QHeaderView::Interactive);
  log_->horizontalHeader()->setStretchLastSection(true);
  log_->setColumnWidth(kCallColumn, 280);

  auto* body = new QSplitter(Qt::Vertical);
  body->addWidget(middle);
  body->addWidget(log_);
  body->setStretchFactor(0, 3);
  body->setStretchFactor(1, 2);

  auto* central = new QWidget;
  auto* column = new QVBoxLayout(central);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  column->addWidget(form_box);
  column->addWidget(body, 1);
  setCentralWidget(central);

  auto* tools = addToolBar(tr("Rehearsal"));
  tools->setObjectName(QStringLiteral("RehearsalToolBar"));
  tools->setMovable(false);
  step_->setShortcut(key(Shortcut::RehearseStep));
  run_to_end_->setShortcut(key(Shortcut::RehearseRunToEnd));
  start_->setToolTip(tr("Rehearse the script from the start, over a copy of the line as it is now"));
  step_->setToolTip(tr("Let the step that waits go on (%1)").arg(step_->shortcut().toString(QKeySequence::NativeText)));
  run_to_end_->setToolTip(tr("Let every step go on, stopping only for a value (%1)")
                              .arg(run_to_end_->shortcut().toString(QKeySequence::NativeText)));
  stop_->setToolTip(tr("End the rehearsal"));
  restart_->setToolTip(tr("Start over, over a new copy of the line, with the same fields"));
  for (QAction* a : {start_, step_, run_to_end_, stop_, restart_}) tools->addAction(a);
  connect(start_, &QAction::triggered, this, [this] { start(); });
  connect(step_, &QAction::triggered, this, [this] { step(); });
  connect(run_to_end_, &QAction::triggered, this, [this] { run_to_end(); });
  connect(stop_, &QAction::triggered, this, [this] { stop(); });
  connect(restart_, &QAction::triggered, this, [this] { restart(); });

  statusBar()->addPermanentWidget(status_, 1);

  confirm_replace_ = [this] {
    return QMessageBox::question(this, tr("Rehearse Script"),
                                 tr("A rehearsal of %1 is running. Stop it and load the other script?").arg(name_),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
  };

  settings_->beginGroup(QStringLiteral("rehearsal_window"));
  if (auto g = settings_->value(QStringLiteral("geometry")).toByteArray(); !g.isEmpty()) restoreGeometry(g);
  settings_->endGroup();
  update_actions();
}

RehearsalWindow::~RehearsalWindow() {
  end_runner();
  // The view reads canvas_state_, a member, which goes before the children.
  delete canvas_;
  canvas_ = nullptr;
}

void RehearsalWindow::build_fields() {
  const scripting::ValueMap defaults = scripting::default_context();
  const auto add = [this](QFormLayout* form, const std::string& name, const scripting::Value& value) {
    QWidget* editor = nullptr;
    if (std::holds_alternative<bool>(value)) {
      editor = new QCheckBox;
    } else {
      auto* line = new QLineEdit;
      if (std::holds_alternative<std::monostate>(value)) line->setPlaceholderText(tr("None"));
      editor = line;
    }
    editor->setObjectName(QStringLiteral("field_") + q(name));
    form->addRow(q(name), editor);
    field_list_.push_back(Field{name, value, editor});
  };
  for (const char* name : kFirstFields)
    if (const auto it = defaults.find(name); it != defaults.end()) add(fields_, it->first, it->second);
  for (const auto& [name, value] : defaults)
    if (std::find(kFirstFields.begin(), kFirstFields.end(), name) == kFirstFields.end()) add(more_fields_, name, value);
}

void RehearsalWindow::fill_fields(const scripting::ValueMap& values) {
  for (const Field& f : field_list_) {
    const auto it = values.find(f.name);
    const scripting::Value v = it != values.end() ? it->second : f.kind;
    if (auto* box = qobject_cast<QCheckBox*>(f.editor)) {
      const auto* b = std::get_if<bool>(&v);
      box->setChecked(b != nullptr && *b);
    } else if (auto* line = qobject_cast<QLineEdit*>(f.editor)) {
      line->setText(field_text(v));
    }
  }
}

scripting::ValueMap RehearsalWindow::field_values() const {
  scripting::ValueMap out;
  for (const Field& f : field_list_) {
    if (const auto* box = qobject_cast<const QCheckBox*>(f.editor)) out[f.name] = box->isChecked();
    else if (const auto* line = qobject_cast<const QLineEdit*>(f.editor)) out[f.name] = read_text(line->text(), f.kind);
  }
  return out;
}

void RehearsalWindow::set_script(scripting::ScriptKind kind, QString name, QString text, scripting::ValueMap defaults) {
  if (running() && !confirm_replace_()) return;
  end_runner();
  kind_ = kind;
  name_ = std::move(name);
  text_ = std::move(text);
  loaded_ = true;
  setWindowTitle(tr("Rehearse %1").arg(name_));
  delete highlighter_;
  highlighter_ = new ScriptHighlighter(source_->document(), kind_);
  fill_fields(scripting::make_context(std::move(defaults)).globals);
  source_->setPlainText(text_);
  shown_script_ = name_;
  stack_->setVisible(false);
  source_->set_current_line(0);
  log_->setRowCount(0);
  canvas_state_.reset();
  for (const auto& [valve, state] : canvas_state_.state().valves)
    if (ValveItem* item = canvas_->valve(valve)) item->set_marked(false);
  update_actions();
}

void RehearsalWindow::start() {
  if (!loaded_) return;
  end_runner();
  log_->setRowCount(0);
  source_->setPlainText(text_);
  shown_script_ = name_;
  stack_->setVisible(false);
  source_->set_current_line(0);
  canvas_state_.reset();
  for (const auto& [valve, state] : canvas_state_.state().valves)
    if (ValveItem* item = canvas_->valve(valve)) item->set_marked(false);

  scripting::Script script{name_.toStdString(), text_.toStdString(), kind_};
  scripting::ScriptEnvironment env = experiment::lab::editor_environment(lab_);
  env.context = scripting::make_context(field_values());

  // The check first: a script that fails it starts nothing, and its
  // diagnostics are listed as the editor's Problems are.
  auto check = host_.check(script, env);
  if (!check) {
    add_row({}, {}, {}, q(check.error().what), true);
    update_actions();
    return;
  }
  if (!check->ok()) {
    for (const auto& d : check->errors()) {
      QString text = tr("error: %1").arg(q(d.message));
      if (!d.code.empty()) text += QStringLiteral(" [") + q(d.code) + QStringLiteral("]");
      add_row({}, QStringLiteral("%1:%2").arg(q(d.script.empty() ? script.name : d.script)).arg(d.line), {}, text,
              true);
    }
    update_actions();
    return;
  }

  LineSnapshot snapshot = rehearsal_snapshot(bridge_.state(), interlocks_);
  canvas_state_.apply(snapshot);
  const unsigned run = ++run_;
  // Both arrive on the runner's thread; queued to this one, where the run
  // they belong to is checked.
  auto on_step = [this, run](const StepRecord& record) {
    QMetaObject::invokeMethod(this, [this, run, record] { on_record(run, record); }, Qt::QueuedConnection);
  };
  auto on_done = [this, run](const Result<scripting::ScriptResult>& result) {
    QMetaObject::invokeMethod(this, [this, run, result] { on_finished(run, result); }, Qt::QueuedConnection);
  };
  runner_ = std::make_unique<experiment::rehearsal::RehearsalRunner>(host_, std::move(script), std::move(env),
                                                                     std::move(snapshot), std::move(on_step),
                                                                     std::move(on_done));
  update_actions();
}

bool RehearsalWindow::step_enabled() const { return step_->isEnabled(); }

void RehearsalWindow::step() {
  if (!runner_ || waiting_ != Waiting::Step) return;
  waiting_ = Waiting::None;  // until the next stop says it waits
  update_actions();
  runner_->rehearsal().advance(experiment::rehearsal::Advance::Step);
}

void RehearsalWindow::run_to_end() {
  if (!runner_ || waiting_ != Waiting::Step) return;
  waiting_ = Waiting::None;
  update_actions();
  runner_->rehearsal().advance(experiment::rehearsal::Advance::RunToEnd);
}

void RehearsalWindow::stop() {
  if (!runner_) return;
  end_runner();
  // The step it stood at did not happen.
  if (const int last = log_->rowCount() - 1; last >= 0) {
    log_->removeCellWidget(last, kOutcomeColumn);
    const QTableWidgetItem* item = log_->item(last, kOutcomeColumn);
    if (item == nullptr || item->text().isEmpty()) {
      auto* stopped = new QTableWidgetItem(tr("stopped"));
      stopped->setForeground(theme().muted_text);
      log_->setItem(last, kOutcomeColumn, stopped);
    }
  }
  update_actions();
}

void RehearsalWindow::restart() {
  stop();
  start();
}

void RehearsalWindow::end_runner() {
  ++run_;  // whatever the old run still has queued is dropped
  waiting_ = Waiting::None;
  runner_.reset();  // stops and joins; on this thread, never the runner's
}

int RehearsalWindow::add_row(const QString& index, const QString& where, const QString& call, const QString& outcome,
                             bool error) {
  const int row = log_->rowCount();
  log_->insertRow(row);
  const std::array<QString, 4> cells{index, where, call, outcome};
  for (int c = 0; c < 4; ++c) {
    auto* item = new QTableWidgetItem(cells[static_cast<std::size_t>(c)]);
    if (error) item->setForeground(theme().error_text);
    log_->setItem(row, c, item);
  }
  log_->scrollToBottom();
  return row;
}

void RehearsalWindow::on_record(unsigned run, const StepRecord& record) {
  if (run != run_ || !runner_) return;
  const QString index = QString::number(record.index);
  // A step's row: made when it first reports, filled when it reports again.
  int row = -1;
  for (int r = log_->rowCount() - 1; r >= 0 && row < 0; --r)
    if (const QTableWidgetItem* item = log_->item(r, kIndexColumn); item != nullptr && item->text() == index) row = r;
  const QString where = QStringLiteral("%1:%2").arg(q(record.call.script)).arg(record.call.line);
  if (row < 0) row = add_row(index, where, call_text(record.call), {}, false);
  log_->removeCellWidget(row, kOutcomeColumn);
  log_->item(row, kOutcomeColumn)->setText(q(record.outcome));
  for (int c = 0; c < 4; ++c)
    if (QTableWidgetItem* item = log_->item(row, c))
      item->setForeground(record.refused ? theme().error_text : theme().text);

  if (record.waiting == Waiting::Value) {
    auto* entry = new QLineEdit;
    entry->setObjectName(QStringLiteral("RehearsalValue"));
    entry->setPlaceholderText(record.call.command == "is_enabled" ? tr("a number: non-zero is true, 0 false")
                                                                  : tr("value, then Enter"));
    log_->setCellWidget(row, kOutcomeColumn, entry);
    connect(entry, &QLineEdit::returnPressed, this, [this, row, entry] { provide_value(row, entry); });
    entry->setFocus();
  }
  waiting_ = record.waiting;
  show_source(record.call);

  // What the copy holds now; what the step changed is outlined.
  canvas_state_.apply(runner_->rehearsal().state());
  if (record.waiting == Waiting::None) {
    for (const auto& [valve, state] : canvas_state_.state().valves) {
      if (ValveItem* item = canvas_->valve(valve))
        item->set_marked(std::find(record.changed.begin(), record.changed.end(), valve) != record.changed.end());
    }
  }
  log_->scrollToBottom();
  update_actions();
}

void RehearsalWindow::on_finished(unsigned run, const Result<scripting::ScriptResult>& result) {
  if (run != run_ || !runner_) return;
  if (result) {
    add_row({}, {}, {}, tr("finished"), false);
  } else if (result.error().kind != ErrorKind::Cancelled) {
    add_row({}, {}, {}, q(result.error().what), true);
  }
  end_runner();  // the thread has returned from its last call
  update_actions();
}

void RehearsalWindow::provide_value(int row, QLineEdit* entry) {
  if (!runner_ || waiting_ != Waiting::Value) return;
  bool ok = false;
  const double v = entry->text().trimmed().toDouble(&ok);
  style::set_invalid(entry, !ok);
  if (!ok) return;
  waiting_ = Waiting::None;
  entry->setEnabled(false);
  log_->item(row, kOutcomeColumn)->setText(QStringLiteral("= ") + number_text(v));
  update_actions();
  runner_->rehearsal().provide_value(v);
}

void RehearsalWindow::show_source(const scripting::StepCall& call) {
  const QString script = q(call.script);
  if (!script.isEmpty() && script != shown_script_) {
    if (script == name_) {
      source_->setPlainText(text_);
    } else if (lab_.scripts) {
      if (auto found = lab_.scripts->resolver().resolve(call.script, kind_)) source_->setPlainText(q(found->text));
    }
    shown_script_ = script;
  }
  QStringList stack;
  for (const auto& s : call.gosubs) stack << q(s);
  stack_->setVisible(!stack.isEmpty());
  if (!stack.isEmpty()) stack_->setText(stack.join(QStringLiteral(" › ")) + QStringLiteral(" › ") + script);
  source_->set_current_line(call.line);
}

void RehearsalWindow::update_actions() {
  start_->setEnabled(loaded_ && !running());
  step_->setEnabled(running() && waiting_ == Waiting::Step);
  run_to_end_->setEnabled(running() && waiting_ == Waiting::Step);
  stop_->setEnabled(running());
  restart_->setEnabled(loaded_);
}

QString RehearsalWindow::status_text() const { return status_->text(); }

void RehearsalWindow::closeEvent(QCloseEvent* event) {
  stop();
  settings_->beginGroup(QStringLiteral("rehearsal_window"));
  settings_->setValue(QStringLiteral("geometry"), saveGeometry());
  settings_->endGroup();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
