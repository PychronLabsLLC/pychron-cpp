// elctl exp run's database, with persistence: an experiment::store::Acquisition.

#include "exp_store.hpp"

#include <utility>

#include "pychron/experiment/store/store_persister.hpp"

#ifndef PYCHRON_ELCTL_VERSION
#define PYCHRON_ELCTL_VERSION "0"
#endif

namespace elctl {

namespace {

class AcquisitionStore final : public ExpStore {
 public:
  explicit AcquisitionStore(std::unique_ptr<pychron::experiment::store::Acquisition> acquisition)
      : acquisition_(std::move(acquisition)) {}
  pychron::experiment::persist::IAnalysisPersister& persister() override { return acquisition_->persister(); }

 private:
  std::unique_ptr<pychron::experiment::store::Acquisition> acquisition_;
};

}  // namespace

pychron::Result<std::unique_ptr<ExpStore>> open_exp_store(const std::string& url, const std::filesystem::path& data) {
  auto opened = pychron::experiment::store::open_acquisition(url, data, std::string("pychron-cpp ") + PYCHRON_ELCTL_VERSION);
  if (!opened) return pychron::fail(std::move(opened).error());
  return std::unique_ptr<ExpStore>(std::make_unique<AcquisitionStore>(std::move(*opened)));
}

bool saves_simulated_runs(const std::string& url, bool from_db_option, bool sim_save) {
  return pychron::experiment::store::saves_simulated_runs(url, from_db_option, sim_save);
}

std::string show_database(const std::string& url) { return pychron::experiment::store::without_password(url); }

}  // namespace elctl
