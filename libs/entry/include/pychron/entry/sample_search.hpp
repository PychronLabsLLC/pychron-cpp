#pragma once

// The near-duplicate check made before a new sample is added (sample and
// package entry spec, section 6): it warns and never blocks (legacy
// sample_entry.py:948-960).

#include <string>
#include <string_view>
#include <vector>

#include "pychron/persistence/catalog.hpp"

namespace pychron::entry {

// `name` lowercased without spaces, '-' and '_': "FC 2", "fc-2" and "FC_2"
// all give "fc2".
std::string sample_key(std::string_view name);

// The samples, in any project, whose key equals `name`'s.
std::vector<persistence::SampleRow> near_duplicates(std::string_view name,
                                                    const std::vector<persistence::SampleRow>& samples);

}  // namespace pychron::entry
