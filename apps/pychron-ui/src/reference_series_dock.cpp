#include "reference_series_dock.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

#include <QDateTime>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "error_report.hpp"
#include "options_editor.hpp"
#include "preset_bar.hpp"
#include "pychron/processing/time_series.hpp"
#include "scene_view.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr const char* kLastNKey = "experiment_window/reference_series/last_n";

QString preset_key(ReferenceKind kind) {
  return QStringLiteral("experiment_window/reference_series/%1/preset")
      .arg(kind == ReferenceKind::Airs ? QStringLiteral("airs") : QStringLiteral("blanks"));
}

QString starting(ReferenceKind kind) { return QString::fromUtf8(starting_preset(kind)); }

// The two reads again after a saved run: 2 s after it, then 10 s after it.
constexpr std::array<std::chrono::seconds, 2> kRetryDelays{std::chrono::seconds(2), std::chrono::seconds(8)};

}  // namespace

ReferenceSeriesDock::ReferenceSeriesDock(Deps deps, QWidget* parent) : QWidget(parent), deps_(std::move(deps)) {
  setObjectName(QStringLiteral("ReferenceSeriesDock"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);

  auto* header = new QHBoxLayout;
  header->addWidget(new QLabel(tr("Last")));
  last_n_ = new QSpinBox;
  last_n_->setObjectName(QStringLiteral("ReferenceLastN"));
  last_n_->setRange(kMinLastN, kMaxLastN);
  last_n_->setToolTip(tr("How many of the newest references each tab draws"));
  int n = kDefaultLastN;
  if (deps_.settings) n = deps_.settings->value(QString::fromLatin1(kLastNKey), kDefaultLastN).toInt();
  last_n_->setValue(std::clamp(n, kMinLastN, kMaxLastN));
  header->addWidget(last_n_);
  reset_ = new QPushButton(tr("Reset"));
  reset_->setObjectName(QStringLiteral("ReferenceReset"));
  reset_->setToolTip(tr("From now: only analyses that start after this moment"));
  header->addWidget(reset_);
  refresh_ = new QPushButton(tr("Refresh"));
  refresh_->setObjectName(QStringLiteral("ReferenceRefresh"));
  header->addWidget(refresh_);
  status_ = new QLabel;
  status_->setObjectName(QStringLiteral("ReferenceStatus"));
  header->addWidget(status_, 1);
  layout->addLayout(header);

  tab_widget_ = new QTabWidget;
  tab_widget_->setObjectName(QStringLiteral("ReferenceTabs"));
  layout->addWidget(tab_widget_, 1);

  const auto& schema = pp::time_series_schema();
  for (const auto kind : {ReferenceKind::Blanks, ReferenceKind::Airs}) {
    Tab& t = tab(kind);
    t.kind = kind;
    t.channel = deps_.bridge->new_channel();
    t.figure = pp::Options(schema);
    t.view = new SceneView;
    const bool airs = kind == ReferenceKind::Airs;
    t.view->setObjectName(airs ? QStringLiteral("ReferenceAirsView") : QStringLiteral("ReferenceBlanksView"));
    const int index = tab_widget_->addTab(t.view, airs ? tr("Airs") : tr("Blanks"));
    t.options_button = new QToolButton;
    t.options_button->setObjectName(airs ? QStringLiteral("ReferenceAirsOptions")
                                         : QStringLiteral("ReferenceBlanksOptions"));
    t.options_button->setText(QStringLiteral("…"));
    t.options_button->setAutoRaise(true);
    t.options_button->setToolTip(tr("Options and presets of this tab"));
    tab_widget_->tabBar()->setTabButton(index, QTabBar::RightSide, t.options_button);
    connect(t.options_button, &QToolButton::clicked, this, [this, kind] {
      auto* dialog = options_dialog(kind);
      dialog->show();
      dialog->raise();
    });
    QString name = starting(kind);
    if (deps_.settings) name = deps_.settings->value(preset_key(kind), name).toString();
    load_preset(t, name);
  }

  connect(last_n_, &QSpinBox::valueChanged, this, [this](int value) {
    if (deps_.settings) deps_.settings->setValue(QString::fromLatin1(kLastNKey), value);
    if (started_) read();
  });
  connect(reset_, &QPushButton::clicked, this, [this] { reset(); });
  connect(refresh_, &QPushButton::clicked, this, [this] { read(); });
}

ReferenceSeriesDock::~ReferenceSeriesDock() {
  // A result still on its way is dropped by the bridge: its channel is gone.
  for (const auto& t : tabs_) deps_.bridge->release(t.channel);
}

ReferenceSeriesDock::Tab& ReferenceSeriesDock::tab(ReferenceKind kind) {
  return tabs_[kind == ReferenceKind::Airs ? 1 : 0];
}

const ReferenceSeriesDock::Tab& ReferenceSeriesDock::tab(ReferenceKind kind) const {
  return tabs_[kind == ReferenceKind::Airs ? 1 : 0];
}

// The preset `name`, or the tab's starting one when there is no such preset
// any more, or the schema's defaults when neither loads.
void ReferenceSeriesDock::load_preset(Tab& t, const QString& name) {
  const auto& schema = pp::time_series_schema();
  for (const QString& candidate : {name, starting(t.kind)}) {
    if (auto loaded = deps_.presets->load(schema, candidate.toStdString())) {
      t.figure = loaded->options;
      t.preset = candidate;
      return;
    }
  }
  t.figure = deps_.presets->defaults(schema);
  t.preset = starting(t.kind);
}

std::optional<double> ReferenceSeriesDock::now() const {
  if (!deps_.wall_now) return std::nullopt;
  return deps_.wall_now();
}

void ReferenceSeriesDock::read() {
  started_ = true;
  for (auto& t : tabs_) submit(t);
}

void ReferenceSeriesDock::reset() {
  since_ = now();
  read();
}

void ReferenceSeriesDock::set_last_n(int n) {
  const int clamped = std::clamp(n, kMinLastN, kMaxLastN);
  if (clamped == last_n_->value()) {
    if (deps_.settings) deps_.settings->setValue(QString::fromLatin1(kLastNKey), clamped);
    return;
  }
  last_n_->setValue(clamped);  // keeps it and reads (valueChanged)
}

int ReferenceSeriesDock::last_n() const { return last_n_->value(); }

void ReferenceSeriesDock::run_saved() {
  ++saved_serial_;
  awaiting_new_ = true;
  retries_made_ = 0;
  read();
}

void ReferenceSeriesDock::submit(Tab& t) {
  ReferenceQuery query;
  query.kind = t.kind;
  if (deps_.mass_spectrometer) query.mass_spectrometer = deps_.mass_spectrometer();
  query.last_n = last_n_->value();
  query.since = since_;
  t.pending = true;
  t.changed = false;
  t.error.reset();
  const QPointer<ReferenceSeriesDock> self(this);
  const ReferenceKind kind = t.kind;
  deps_.bridge->submit(
      t.channel, reference_pipeline(deps_.bridge->domain().units(), query, t.figure), {kReferenceFigure},
      [self, kind](const PipelineResult& r) {
        if (self) self->on_result(kind, r);
      },
      SubmitOptions{.refresh_source = true, .now = now()});
}

void ReferenceSeriesDock::on_result(ReferenceKind kind, const PipelineResult& r) {
  Tab& t = tab(kind);
  t.pending = false;
  if (r.outputs.empty() || !r.outputs[0]) {
    // The last figure stays; the header says why there is no newer one.
    t.error = r.outputs.empty() ? tr("nothing computed") : qs(r.outputs[0].error().what);
    log_shown("reference_fit", tr("Compute"), *t.error);
  } else if (const auto* scene = std::get_if<pp::ScenePtr>(&r.outputs[0]->at(0)); scene && *scene) {
    // The channel's runner gives back the scene it drew before when nothing
    // it reads has changed: then nothing is drawn again.
    if (*scene != t.view->scene()) {
      t.view->set_scene(*scene);
      t.changed = true;
    }
  } else {
    t.error = tr("no figure");
  }
  if (std::ranges::none_of(tabs_, &Tab::pending)) finished();
}

void ReferenceSeriesDock::finished() {
  const auto failed = std::ranges::find_if(tabs_, [](const Tab& t) { return t.error.has_value(); });
  if (failed != tabs_.end()) {
    status_->setText(tr("could not read: %1").arg(*failed->error));
  } else if (const auto t = now()) {
    const auto ms = static_cast<qint64>(std::llround(*t * 1000.0));
    status_->setText(tr("read %1").arg(QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss"))));
  } else {
    status_->setText(tr("read"));
  }

  if (awaiting_new_) {
    const bool found = std::ranges::any_of(tabs_, &Tab::changed);
    if (found || failed != tabs_.end() || !deps_.later ||
        retries_made_ >= kRetryDelays.size()) {
      awaiting_new_ = false;
    } else {
      const auto delay = kRetryDelays[retries_made_];
      ++retries_made_;
      const QPointer<ReferenceSeriesDock> self(this);
      const unsigned serial = saved_serial_;
      deps_.later(delay, [self, serial] {
        if (self && self->saved_serial_ == serial && self->awaiting_new_) self->read();
      });
    }
  }
  emit read_finished();
}

void ReferenceSeriesDock::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (shown_) return;
  shown_ = true;
  read();
}

SceneView* ReferenceSeriesDock::view(ReferenceKind kind) const { return tab(kind).view; }

QString ReferenceSeriesDock::status() const { return status_->text(); }

QString ReferenceSeriesDock::preset(ReferenceKind kind) const { return tab(kind).preset; }

const pp::Options& ReferenceSeriesDock::figure_options(ReferenceKind kind) const { return tab(kind).figure; }

QDialog* ReferenceSeriesDock::options_dialog(ReferenceKind kind) {
  Tab& t = tab(kind);
  if (t.dialog) return t.dialog;
  auto* dialog = new QDialog(this);
  dialog->setObjectName(kind == ReferenceKind::Airs ? QStringLiteral("ReferenceAirsOptionsDialog")
                                                    : QStringLiteral("ReferenceBlanksOptionsDialog"));
  dialog->setWindowTitle(kind == ReferenceKind::Airs ? tr("Airs: options") : tr("Blanks: options"));
  dialog->resize(420, 600);
  auto* layout = new QVBoxLayout(dialog);
  auto* bar = new PresetBar(*deps_.presets, pp::time_series_schema());
  bar->current = [this, kind] { return tab(kind).figure; };
  layout->addWidget(bar);
  auto* editor = new OptionsEditor;
  editor->set_domain(&deps_.bridge->domain());
  layout->addWidget(editor, 1);
  auto* message = new QLabel;
  message->setWordWrap(true);
  layout->addWidget(message);
  editor->set_options(t.figure);
  bar->reload(t.preset);

  connect(bar, &PresetBar::loaded, this, [this, kind, editor](const pp::Options& o, const QString& name) {
    Tab& tb = tab(kind);
    tb.figure = o;
    tb.preset = name;
    if (deps_.settings) deps_.settings->setValue(preset_key(kind), name);
    editor->set_options(o);
    submit(tb);
  });
  connect(bar, &PresetBar::message, message, [message](const QString& text, const QString& details) {
    message->setText(text);
    message->setToolTip(details);
  });
  connect(editor, &OptionsEditor::changed, this, [this, kind, editor] {
    Tab& tb = tab(kind);
    tb.figure = editor->options();
    submit(tb);
  });
  t.dialog = dialog;
  return dialog;
}

}  // namespace pychron::ui
