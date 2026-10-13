// elctl exp run without the DVC store (PYCHRON_PERSISTENCE=OFF, or Qt not
// found): a database cannot be opened, and the command says why.

#include "exp_store.hpp"

namespace elctl {

pychron::Result<std::unique_ptr<ExpStore>> open_exp_store(const std::string&, const std::filesystem::path&) {
  return pychron::fail(pychron::ErrorKind::Config, "built without the DVC store (PYCHRON_PERSISTENCE=OFF or no Qt Sql)");
}

bool saves_simulated_runs(const std::string&, bool, bool) { return true; }

std::string show_database(const std::string& url) { return url; }

}  // namespace elctl
