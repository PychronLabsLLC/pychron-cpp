#include "database_choice.hpp"

#include "pychron/setup/doctor.hpp"
#ifdef PYCHRON_UI_HAS_STORE
// Qt's `signals` keyword macro would rewrite CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/experiment/store/store_persister.hpp"
#pragma pop_macro("signals")
#endif

namespace pychron::ui {

Result<std::optional<std::string>> choose_database(const CommandLine& cli, const setup::SiteInstall* install) {
  if (cli.db) return cli.db;
  if (install == nullptr || install->database.empty()) return std::nullopt;
  auto url = setup::database_url(*install);
  if (!url) return fail(url.error());
  return std::optional<std::string>(std::move(*url));
}

#ifdef PYCHRON_UI_HAS_STORE
RunSaving choose_run_saving(const CommandLine& cli, const std::string& database, bool simulated,
                            const std::filesystem::path& data) {
  if (!simulated || experiment::store::saves_simulated_runs(database, cli.db.has_value(), cli.sim_save)) return {};
  return RunSaving{false, "simulated runs are written to " + (data / "records").string() + ", not to " +
                              experiment::store::without_password(database) + "; pass --sim-save to save them there"};
}
#endif

}  // namespace pychron::ui
