#pragma once

// Irradiation holders from legacy holder files (sample and package entry
// spec, section 6, holder_import.hpp).

#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

// The holder a legacy file describes ("<shape>,<radius>[,<has hole numbers>]"
// then one hole per line). Hole ids must be unique.
Result<persistence::HolderValue> read_holder(std::string_view text);

// Writes `holder` as the next value of irradiation holder `name` (creating the
// reference on first use). Returns the reference object.
Result<persistence::Uuid> save_holder(persistence::IStore& store, const persistence::Actor& actor,
                                      const std::string& name, const persistence::HolderValue& holder);

// The value at the head of a holder reference; nullopt when it has none.
Result<std::optional<persistence::HolderValue>> load_holder(persistence::IStore& store, persistence::Uuid holder);

}  // namespace pychron::entry
