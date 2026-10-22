#pragma once

// Private to libs/publish: the two steps of a build that a destination other
// than the release directory shares (publication workflow design, section
// 5.2): which analyses a release holds, and how a set of loaded analyses is
// reduced.

#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/publish/build.hpp"

namespace pychron::publish::detail {

// The analyses the release's bookmark pins (reference values and interpreted
// ages are pinned too and are not members), in uuid order.
Result<std::vector<persistence::Uuid>> release_members(persistence::IStore& store, const persistence::ReleaseRow& release);

// The `select` then `reduce` pipeline over `loaded`, with the release's
// constants and decay-error option; with `group`, the `group` step as well,
// by the release's `group_by`. Diagnostics of the run are appended to
// `diagnostics` when given.
Result<processing::DatasetPtr> reduce_loaded(const std::vector<processing::AnalysisPtr>& loaded,
                                             const ReleaseOptions& options, bool group = false,
                                             std::vector<std::string>* diagnostics = nullptr);

}  // namespace pychron::publish::detail
