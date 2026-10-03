#pragma once

// What the app's setup paths share: opening a database for the setup wizard
// and doctor, and switching to another install.

#include <filesystem>
#include <ostream>
#include <string>

#include "setup_wizard.hpp"

namespace pychron::ui {

// Opens a DVC store URL (create: migrating it) and describes its schema;
// empty when built without the store.
SetupWizard::OpenDatabase database_opener();

// Starts pychron-ui again on install `name` (detached). False when it could
// not be started.
bool start_install(const std::string& name);

// pychron-ui --self-test: what an installed copy needs, checked without a
// window: the shipped profiles load and resolve, the setup wizard builds, and
// (with the store) an in-memory database opens through the Qt SQL plugin.
// One line per check on `out`; 0 when all pass.
int self_test(std::ostream& out);

// pychron-ui --write-icons: the application icon as pychron-<size>.png for
// the sizes the installers' icon files need (tools/make_icons.py packs them
// into .icns and .ico). 0 when every file was written.
int write_icons(const std::filesystem::path& dir, std::ostream& out);

}  // namespace pychron::ui
