#include "data_main_window.hpp"

#include "brand.hpp"
#include "command_palette.hpp"
#include "menu_hub.hpp"
#include "shortcuts.hpp"

#include <utility>

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>

namespace pychron::ui {

DataMainWindow::DataMainWindow(processing::IAnalysisSource& source, processing::PresetStore& presets,
                               const QString& title, QWidget* parent)
    : QMainWindow(parent),
      data_(new DataWorkspace(this, [this](const QString& line) { statusBar()->showMessage(line, 10000); })),
      installations_(new QAction(tr("Installations…"), this)),
      preferences_(new QAction(tr("Preferences…"), this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(title));
  data_->set_source(&source, &presets);
  setCentralWidget(data_->browser(this));

  preferences_->setShortcut(key(Shortcut::Preferences));
  preferences_->setMenuRole(QAction::PreferencesRole);
  connect(preferences_, &QAction::triggered, this, [this] { open_preferences(); });
  auto* quit = new QAction(tr("Quit"), this);
  quit->setShortcut(key(Shortcut::Quit));
  quit->setMenuRole(QAction::QuitRole);
  connect(quit, &QAction::triggered, this, &QMainWindow::close);
  MenuHub::instance().contribute(this, MenuHub::Menu::File, {installations_, preferences_}, MenuHub::Scope::App);
  MenuHub::instance().contribute(this, MenuHub::Menu::File, {quit}, MenuHub::Scope::App);
  installations_->setVisible(false);
  connect(installations_, &QAction::triggered, this, [this] {
    if (on_installations_) on_installations_();
  });
  MenuHub::instance().contribute(this, MenuHub::Menu::Help,
                                 {make_command_palette_action(this), make_shortcuts_action(this)}, MenuHub::Scope::App);
  about_ = brand::add_help_menu(this);
  MenuHub::instance().install(this);
}

DataMainWindow::~DataMainWindow() {
  takeCentralWidget();  // the workspace deletes the browser with the other data windows
  data_->set_source(nullptr, nullptr);
}

void DataMainWindow::set_installations_handler(std::function<void()> handler) {
  on_installations_ = std::move(handler);
  installations_->setVisible(static_cast<bool>(on_installations_));
}

void DataMainWindow::set_preferences_settings(PreferencesDialog::SettingsFactory settings) {
  preferences_settings_ = std::move(settings);
}

PreferencesDialog* DataMainWindow::open_preferences() {
  return PreferencesDialog::show_for(this, preferences_dialog_, preferences_settings_, std::nullopt,
                                     [this](const PreferencesDialog::Values& values) {
                                       apply_preferences(values.preferences);
                                     });
}

void DataMainWindow::apply_preferences(const Preferences& preferences) {
  apply_application_preferences(preferences);
  data_->set_page_size(preferences.browser_page_size);
}

}  // namespace pychron::ui
