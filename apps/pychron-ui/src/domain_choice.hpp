#pragma once

// The domain pychron-ui runs on: the install's `domain` key, else "arar".

#include <string>
#include <string_view>

#include "pychron/processing/domain.hpp"

namespace pychron::ui {

// Registers the domains this build links and gives the one named `id` (an
// install's `domain`; "arar" without an install). Null, with the reason on
// `error`, when no domain has that id (the install file's mistake) or when the
// registry disagrees with setup::known_domains() (this program's own).
const processing::Domain* choose_domain(processing::DomainRegistry& registry, std::string_view id,
                                        std::string& error);

}  // namespace pychron::ui
