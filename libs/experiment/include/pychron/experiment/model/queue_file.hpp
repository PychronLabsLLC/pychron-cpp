#pragma once

#include <string>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

// Canonical experiment.toml text for `q`: [queue] (with schema_version) then one
// [[runs]] per run in a fixed key order, default-valued fields omitted, so
// parse_queue(dump_queue(q)) == q and dumps diff cleanly. A run whose device
// equals the queue's extract_device does not repeat it.
std::string dump_queue(const QueueSpec& q);

// Reads and parses (Io error if unreadable, Config error if invalid).
Result<QueueSpec> load_queue_file(const std::string& path, const IdentifierRules& ids);

// Writes dump_queue(q) to `path` via a temporary file and rename.
Result<void> save_queue_file(const std::string& path, const QueueSpec& q);

}  // namespace pychron::experiment
