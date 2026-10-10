#include "no_database_persister.hpp"

namespace pychron::ui {

NoDatabasePersister::NoDatabasePersister(const std::string& reason)
    : why_("the database cannot be reached: " + reason) {}

Result<int> NoDatabasePersister::next_aliquot(const std::string&) { return fail(ErrorKind::Io, why_); }

Result<void> NoDatabasePersister::begin_run(const experiment::RunIdentity&, const experiment::QueueSpec&) {
  return fail(ErrorKind::Io, why_);
}

Result<void> NoDatabasePersister::save_extraction(const experiment::record::AnalysisRecord&) {
  return fail(ErrorKind::Io, why_);
}

Result<void> NoDatabasePersister::save_analysis(const experiment::record::AnalysisRecord&) {
  return fail(ErrorKind::Io, why_);
}

Result<void> NoDatabasePersister::save_artifact(const std::string&, const std::string&,
                                                const std::vector<std::uint8_t>&) {
  return fail(ErrorKind::Io, why_);
}

Result<std::vector<std::string>> NoDatabasePersister::check_queue(const experiment::QueueSpec&) {
  return std::vector<std::string>{why_};
}

}  // namespace pychron::ui
