#pragma once

// The "rewrites" member of a changeset's provenance detail (legacy ingestion
// spec, section 10, items 11 and 16): the files a commit rewrote without a
// revision. One commit's notes can reach the writer in more than one batch,
// so they are merged into what the row already holds.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/ingest/batch.hpp"

namespace pychron::ingest::detail {

// The detail that holds `notes` under "rewrites", one entry per path, sorted
// by path. It is built on `stored`, the detail the row has now (empty: there
// is no row, or it has no detail), keeping its other members and the entries
// it already has; without `stored` it is built on `base`, a JSON object.
// nullopt: `stored` already has an entry for every note, so there is nothing
// to write. The texts are not interpreted beyond their structure: a number
// keeps every digit it was written with.
std::optional<std::string> with_rewrites(std::string_view stored, std::string_view base,
                                         const std::vector<FileNote>& notes);

}  // namespace pychron::ingest::detail
