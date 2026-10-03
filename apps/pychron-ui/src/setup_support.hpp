#pragma once

// What the app's setup paths share: opening a database for the setup wizard
// and doctor, and switching to another install.

#include <string>

#include "setup_wizard.hpp"

namespace pychron::ui {

// Opens a DVC store URL (create: migrating it) and describes its schema;
// empty when built without the store.
SetupWizard::OpenDatabase database_opener();

// Starts pychron-ui again on install `name` (detached). False when it could
// not be started.
bool start_install(const std::string& name);

}  // namespace pychron::ui
