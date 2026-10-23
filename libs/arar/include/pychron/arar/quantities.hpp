#pragma once

// The Ar-Ar names and stages a quantity may use (domain seam spec 3.4): what
// ArArReduced answers, with the labels and units figure axes have always
// shown for them.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/processing/domain.hpp"

namespace pychron::arar {

// A name ArArReduced::named() answers (or its alias "f", "rad40_percent"), or
// one of its stages; nullopt otherwise.
[[nodiscard]] std::optional<processing::QuantityInfo> quantity_info(std::string_view name);

// ArArReduced's names, in order, without the aliases.
[[nodiscard]] std::vector<std::string> value_names();

// What the Ar-Ar domain offers as quantities (Domain::quantities()), in
// order: the values reduce() computes, then the two stages. Not "j".
[[nodiscard]] std::vector<std::string> listed_quantities();

// "F" for "f", "radiogenic_yield" for "rad40_percent"; any other name as given.
[[nodiscard]] std::string_view canonical_name(std::string_view name) noexcept;

}  // namespace pychron::arar
