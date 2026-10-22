#include "pychron/publish/earthbank_source.hpp"

#include "members.hpp"

namespace pychron::publish {

namespace ps = persistence;
namespace pp = processing;

Result<std::vector<pp::AnalysisPtr>> release_dataset(ps::IStore& store, pp::StoreSource& source, ps::Uuid release) {
  auto row = store.release(release);
  if (!row) return fail(row.error());
  if (!*row) return fail(ErrorKind::Config, "no release " + release.str());
  const ps::ReleaseRow& r = **row;
  auto members = detail::release_members(store, r);
  if (!members) return fail(members.error());
  std::vector<pp::AnalysisPtr> loaded;
  loaded.reserve(members->size());
  for (const auto& m : *members) {
    auto analysis = source.load_at(m.str(), r.bookmark.str());
    if (!analysis) return fail(analysis.error());
    loaded.push_back(*analysis);
  }
  return loaded;
}

}  // namespace pychron::publish
