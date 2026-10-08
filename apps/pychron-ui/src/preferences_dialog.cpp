#include "preferences_dialog.hpp"

#include <utility>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "logging_page.hpp"
#include "metrics_page.hpp"
#include "spectrometer_window.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

// A font size field whose step below the smallest size reads "Default" (0).
QSpinBox* font_field() {
  auto* spin = new QSpinBox;
  spin->setRange(Preferences::kMinFontPt - 1, Preferences::kMaxFontPt);
  spin->setSpecialValueText(QObject::tr("Default"));
  spin->setSuffix(QObject::tr(" pt"));
  return spin;
}

int font_value(const QSpinBox* spin) { return spin->value() < Preferences::kMinFontPt ? 0 : spin->value(); }
void set_font_value(QSpinBox* spin, int pt) { spin->setValue(pt < Preferences::kMinFontPt ? spin->minimum() : pt); }

}  // namespace

QLabel* preferences_note(const QString& text) {
  auto* label = new QLabel(text);
  label->setWordWrap(true);
  style::set_tone(label, style::Tone::Muted);
  return label;
}

PreferencesDialog::PreferencesDialog(const Values& current, Apply apply, QWidget* parent)
    : QDialog(parent),
      apply_(std::move(apply)),
      pages_(new QListWidget),
      stack_(new QStackedWidget),
      font_(font_field()),
      code_font_(font_field()),
      page_size_(new QSpinBox),
      problem_(new QLabel),
      buttons_(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply |
                                    QDialogButtonBox::RestoreDefaults)) {
  setWindowTitle(tr("Preferences"));
  resize(560, 340);

  auto* appearance = new QWidget;
  auto* appearance_form = new QFormLayout(appearance);
  appearance_form->addRow(tr("Interface font size:"), font_);
  appearance_form->addRow(tr("Script editor font size:"), code_font_);
  appearance_form->addRow(preferences_note(tr("Default is the system's size; the script editor follows the interface. "
                                  "Window titles change in windows opened afterwards.")));
  add_page(tr("Appearance"), appearance);

  page_size_->setRange(Preferences::kMinPageSize, Preferences::kMaxPageSize);
  page_size_->setSingleStep(50);
  page_size_->setSuffix(tr(" analyses"));
  auto* data_page = new QWidget;
  auto* data_form = new QFormLayout(data_page);
  data_form->addRow(tr("Browser page size:"), page_size_);
  data_form->addRow(preferences_note(tr("How many analyses the data browser loads at a time; Load more fetches the next page.")));
  add_page(tr("Data"), data_page);

  if (current.confirm_move_amu) {
    confirm_move_ = new QDoubleSpinBox;
    confirm_move_->setRange(0.0, 300.0);
    confirm_move_->setDecimals(2);
    confirm_move_->setSingleStep(0.5);
    confirm_move_->setSpecialValueText(tr("Never ask"));
    confirm_move_->setSuffix(tr(" amu"));
    auto* spectrometer = new QWidget;
    auto* form = new QFormLayout(spectrometer);
    form->addRow(tr("Ask before moving the magnet more than:"), confirm_move_);
    form->addRow(preferences_note(tr("The change of mass on the reference detector. A move from an unknown position "
                         "always asks. Kept for this spectrometer.")));
    add_page(tr("Spectrometer"), spectrometer);
  }

  if (current.line) {
    logging_ = new LoggingPage;
    add_page(tr("Logging"), logging_);
    metrics_ = new MetricsPage;
    add_page(tr("Metrics"), metrics_);
    resize(680, 520);  // the logger table needs the room
  }

  problem_->setWordWrap(true);
  style::set_tone(problem_, style::Tone::Error);
  problem_->hide();

  pages_->setFixedWidth(150);
  pages_->setCurrentRow(0);
  connect(pages_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

  auto* body = new QHBoxLayout;
  body->addWidget(pages_);
  body->addWidget(stack_, 1);
  auto* layout = new QVBoxLayout(this);
  layout->addLayout(body, 1);
  layout->addWidget(problem_);
  layout->addWidget(buttons_);

  connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
    if (commit()) accept();
  });
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { commit(); });
  connect(buttons_->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
          [this] { restore_defaults(); });

  set_values(current);
}

bool PreferencesDialog::commit() {
  problem_->hide();
  if (metrics_ != nullptr) {
    if (const QString wrong = metrics_->problem(); !wrong.isEmpty()) {
      complain(wrong, metrics_);
      return false;
    }
  }
  const Values v = values();
  // The line's file first: it is the part that can fail, and nothing else
  // is to change when it does.
  if (v.line && line_save_) {
    if (const QString failed = line_save_(*v.line); !failed.isEmpty()) {
      complain(tr("Logging and metrics were not saved. %1").arg(failed), nullptr);
      return false;
    }
  }
  if (apply_) apply_(v);
  return true;
}

void PreferencesDialog::complain(const QString& what, QWidget* page) {
  problem_->setText(what);
  problem_->show();
  if (page != nullptr) pages_->setCurrentRow(stack_->indexOf(page));
}

PreferencesDialog* PreferencesDialog::show_for(QWidget* window, QPointer<PreferencesDialog>& open,
                                              const SettingsFactory& settings, std::optional<double> confirm_move_amu,
                                              Apply apply, std::optional<LineSettings> line, LineSave line_save) {
  if (open && open->isVisible()) {  // a closed one waits for its deferred delete
    open->raise();
    open->activateWindow();
    return open;
  }
  auto make = [settings] { return settings ? settings() : std::make_unique<QSettings>(); };
  const Values current{load_preferences(*make()), confirm_move_amu, std::move(line)};
  auto* dialog = new PreferencesDialog(
      current,
      [make, then = std::move(apply)](const Values& values) {
        save_preferences(*make(), values.preferences);
        if (then) then(values);
      },
      window);
  dialog->set_line_save(std::move(line_save));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->open();
  open = dialog;
  return dialog;
}

void PreferencesDialog::add_page(const QString& name, QWidget* page) {
  pages_->addItem(name);
  stack_->addWidget(page);
}

PreferencesDialog::Values PreferencesDialog::values() const {
  Values v;
  v.preferences.font_pt = font_value(font_);
  v.preferences.code_font_pt = font_value(code_font_);
  v.preferences.browser_page_size = page_size_->value();
  if (confirm_move_ != nullptr) v.confirm_move_amu = confirm_move_->value();
  if (line_ && logging_ != nullptr && metrics_ != nullptr) {
    v.line = line_;
    v.line->logging = logging_->value();
    v.line->metrics = metrics_->value();
  }
  return v;
}

void PreferencesDialog::set_values(const Values& values) {
  set_font_value(font_, values.preferences.font_pt);
  set_font_value(code_font_, values.preferences.code_font_pt);
  page_size_->setValue(values.preferences.browser_page_size);
  if (confirm_move_ != nullptr && values.confirm_move_amu) confirm_move_->setValue(*values.confirm_move_amu);
  if (values.line && logging_ != nullptr && metrics_ != nullptr) {
    line_ = values.line;
    logging_->set(values.line->logging);
    metrics_->set(values.line->metrics, values.line->metrics_status);
  }
}

void PreferencesDialog::restore_defaults() {
  // For the line's pages the default is what the shared config says: with
  // nothing differing from it, nothing is kept in the local file.
  std::optional<LineSettings> line = line_;
  if (line) {
    line->logging = line->shared_logging;
    line->metrics = line->shared_metrics;
  }
  set_values(Values{Preferences{}, SpectrometerWindow::kDefaultConfirmMoveAmu, std::move(line)});
}

}  // namespace pychron::ui
