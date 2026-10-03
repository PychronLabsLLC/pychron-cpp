#include "setup_support.hpp"

#include <QCoreApplication>
#include <QProcess>
#include <QStringList>

#ifdef PYCHRON_UI_HAS_STORE
#pragma push_macro("signals")
#undef signals
#include "pychron/persistence/store.hpp"
#pragma pop_macro("signals")
#endif

namespace pychron::ui {

SetupWizard::OpenDatabase database_opener() {
#ifdef PYCHRON_UI_HAS_STORE
  return [](const std::string& url, bool create) -> Result<std::string> {
    auto store = persistence::open_store(persistence::StoreConfig{url, create});
    if (!store) return fail(std::move(store).error());
    auto status = (*store)->schema_status();
    if (!status) return fail(std::move(status).error());
    return status->empty() ? std::string("empty schema") : "schema version " + std::to_string(status->back().version);
  };
#else
  return {};
#endif
}

bool start_install(const std::string& name) {
  return QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--install"), QString::fromStdString(name)});
}

}  // namespace pychron::ui
