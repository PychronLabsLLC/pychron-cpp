#include "domain.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

#include "pychron/arar/domain.hpp"
#include "pychron/setup/site.hpp"

namespace elctl {

namespace {

std::string join(const std::vector<std::string>& ids) {
  std::string text;
  for (const auto& id : ids) text += (text.empty() ? "" : ", ") + id;
  return text;
}

}  // namespace

const pychron::processing::Domain* choose_domain(pychron::processing::DomainRegistry& registry, std::string_view id,
                                                 std::ostream& err) {
  registry.add(pychron::processing::make_noble_gas_domain());
  registry.add(pychron::arar::make_domain());
  auto known = pychron::setup::known_domains();
  const auto registered = registry.ids();  // sorted
  std::sort(known.begin(), known.end());
  if (known != registered) {
    err << "error: setup::known_domains() lists " << join(known) << " but the registry holds " << join(registered)
        << '\n';
    return nullptr;
  }
  if (const auto* domain = registry.find(id)) return domain;
  err << "error: unknown domain '" << id << "'; known: " << join(registered) << '\n';
  return nullptr;
}

}  // namespace elctl
