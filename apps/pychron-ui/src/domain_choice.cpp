#include "domain_choice.hpp"

#include <algorithm>
#include <vector>

#include "pychron/arar/domain.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::ui {

namespace {

std::string join(const std::vector<std::string>& ids) {
  std::string text;
  for (const auto& name : ids) text += (text.empty() ? "" : ", ") + name;
  return text;
}

}  // namespace

const processing::Domain* choose_domain(processing::DomainRegistry& registry, std::string_view id,
                                        std::string& error) {
  registry.add(processing::make_noble_gas_domain());
  registry.add(pychron::arar::make_domain());
  auto known = setup::known_domains();
  const auto registered = registry.ids();  // sorted
  std::sort(known.begin(), known.end());
  if (known != registered) {
    error = "setup::known_domains() lists " + join(known) + " but the registry holds " + join(registered);
    return nullptr;
  }
  if (const auto* domain = registry.find(id)) return domain;
  error = "unknown domain '" + std::string(id) + "'; known: " + join(registered);
  return nullptr;
}

}  // namespace pychron::ui
