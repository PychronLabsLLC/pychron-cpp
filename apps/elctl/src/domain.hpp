#pragma once

// The domain an elctl invocation runs on (domain seam spec 4). The UI has its
// own helper of the same name (apps/pychron-ui/src/main.cpp): one per app.

#include <iosfwd>
#include <string_view>

#include "pychron/processing/domain.hpp"

namespace elctl {

// Registers the domains this build links and gives the one named `id`. Null,
// with the reason on `err`, when the registry disagrees with
// setup::known_domains() (this program's own mistake) or holds no domain of
// that id (the install file's, or --domain's).
const pychron::processing::Domain* choose_domain(pychron::processing::DomainRegistry& registry, std::string_view id,
                                                 std::ostream& err);

}  // namespace elctl
