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
  auto opts = parse_release_options(r.options_json);
  if (!opts) return fail(opts.error());
  auto members = detail::release_members(store, r);
  if (!members) return fail(members.error());
  std::vector<pp::AnalysisPtr> loaded;
  loaded.reserve(members->size());
  for (const auto& m : *members) {
    auto analysis = source.load_at(m.str(), r.bookmark.str());
    if (!analysis) return fail(analysis.error());
    loaded.push_back(*analysis);
  }
  auto dataset = detail::reduce_loaded(loaded, *opts);
  if (!dataset) return fail(dataset.error());
  // The reduced analyses, in member order.
  std::vector<pp::AnalysisPtr> out;
  out.reserve(loaded.size());
  for (const auto& l : loaded)
    for (const auto& item : (*dataset)->items())
      if (item.analysis->analysis->uuid == l->uuid) {
        out.push_back(item.analysis->analysis);
        break;
      }
  return out;
}

}  // namespace pychron::publish
