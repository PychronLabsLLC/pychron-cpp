#pragma once

// The database an `exp run` writes its runs to (--db, or the install's), held
// so that exp.cpp need not include the store: with persistence the object
// owns an opened experiment::store::Acquisition, without it opening says why
// it cannot.

#include <filesystem>
#include <memory>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/experiment/persist/persister.hpp"

namespace elctl {

// The persister must outlive every user of it: declare the object that holds
// it before the LabSession, which is then destroyed first.
struct ExpStore {
  ExpStore() = default;
  ExpStore(const ExpStore&) = delete;
  ExpStore& operator=(const ExpStore&) = delete;
  virtual ~ExpStore() = default;
  virtual pychron::experiment::persist::IAnalysisPersister& persister() = 0;
};

// Opens `url` (never migrates) for the acquisition whose data folder is
// `data`. Call from the program's main thread. Without persistence: a Config
// error "built without the DVC store (PYCHRON_PERSISTENCE=OFF or no Qt Sql)".
pychron::Result<std::unique_ptr<ExpStore>> open_exp_store(const std::string& url, const std::filesystem::path& data);

// `url` as it may be shown: without its password.
std::string show_database(const std::string& url);

}  // namespace elctl
