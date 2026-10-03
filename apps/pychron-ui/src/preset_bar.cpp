#include "preset_bar.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QPushButton>
#include <QVBoxLayout>

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
}  // namespace

PresetBar::PresetBar(pp::PresetStore& store, pp::SchemaPtr schema, QWidget* parent)
    : QWidget(parent), store_(store), schema_(std::move(schema)) {
  ask_name = [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save preset"), tr("Name"), QLineEdit::Normal, QString(), &ok);
    return ok ? name.trimmed() : QString();
  };
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  combo_ = new QComboBox;
  layout->addWidget(combo_);
  auto* buttons = new QHBoxLayout;
  auto* save_button = new QPushButton(tr("Save"));
  auto* save_as = new QPushButton(tr("Save as..."));
  auto* remove_button = new QPushButton(tr("Delete"));
  auto* factory = new QPushButton(tr("Factory"));
  factory->setToolTip(tr("Reload the factory preset of this name"));
  for (auto* b : {save_button, save_as, remove_button, factory}) buttons->addWidget(b);
  layout->addLayout(buttons);
  connect(save_button, &QPushButton::clicked, this, [this] { save(false); });
  connect(save_as, &QPushButton::clicked, this, [this] { save(true); });
  connect(remove_button, &QPushButton::clicked, this, [this] { remove(); });
  connect(factory, &QPushButton::clicked, this, [this] { factory_reset(); });
  connect(combo_, &QComboBox::activated, this, [this](int) { select(combo_->currentText()); });
}

QString PresetBar::current_name() const { return combo_->currentText(); }

void PresetBar::reload(const QString& select_name) {
  combo_->clear();
  for (const auto& p : store_.list(schema_)) {
    combo_->addItem(qs(p.name));
    combo_->setItemData(combo_->count() - 1,
                        p.origin == pp::PresetOrigin::Factory ? tr("factory")
                        : p.origin == pp::PresetOrigin::Lab   ? tr("lab")
                                                              : tr("yours"),
                        Qt::ToolTipRole);
  }
  combo_->setCurrentText(select_name);
}

bool PresetBar::select(const QString& name) {
  auto loaded_options = store_.load(schema_, name.toStdString());
  if (!loaded_options) {
    emit message(tr("Preset: %1").arg(qs(loaded_options.error().what)), {});
    return false;
  }
  combo_->setCurrentText(name);
  QStringList warnings;
  for (const auto& w : loaded_options->warnings) warnings << qs(w);
  emit loaded(loaded_options->options, name);
  if (!warnings.isEmpty()) emit message(tr("Preset \"%1\" loaded with warnings").arg(name), warnings.join(QLatin1Char('\n')));
  return true;
}

bool PresetBar::save(bool as) {
  QString name = combo_->currentText();
  if (as || name.isEmpty()) name = ask_name ? ask_name() : QString();
  if (name.isEmpty() || !current) return false;
  if (auto ok = store_.save(name.toStdString(), current()); !ok) {
    emit message(tr("Save failed: %1").arg(qs(ok.error().what)), {});
    return false;
  }
  reload(name);
  emit message(tr("Saved preset \"%1\"").arg(name), {});
  return true;
}

bool PresetBar::remove() {
  const QString name = combo_->currentText();
  if (auto ok = store_.remove(schema_, name.toStdString()); !ok) {
    emit message(qs(ok.error().what), {});
    return false;
  }
  reload(name);  // a factory preset of the same name may remain
  return select(combo_->currentText().isEmpty() ? QStringLiteral("Default") : combo_->currentText());
}

bool PresetBar::factory_reset() {
  const QString name = combo_->currentText();
  auto f = store_.factory(schema_, name.toStdString());
  if (!f) {
    emit message(tr("No factory preset named \"%1\"").arg(name), {});
    return false;
  }
  emit loaded(f->options, name);
  return true;
}

}  // namespace pychron::ui
