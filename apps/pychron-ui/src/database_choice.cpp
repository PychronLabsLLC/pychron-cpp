#include "database_choice.hpp"

#include "pychron/setup/doctor.hpp"

namespace pychron::ui {

Result<std::optional<std::string>> choose_database(const CommandLine& cli, const setup::SiteInstall* install) {
  if (cli.db) return cli.db;
  if (install == nullptr || install->database.empty()) return std::nullopt;
  auto url = setup::database_url(*install);
  if (!url) return fail(url.error());
  return std::optional<std::string>(std::move(*url));
}

}  // namespace pychron::ui
