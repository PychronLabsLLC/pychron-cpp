// Catalog rows by natural key, read-only: the accounting of a catalog source
// (verify_parts.hpp). The keys are those catalog.cpp ensures rows by.

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "verify_parts.hpp"

namespace pychron::ingest::detail {

namespace P = pychron::persistence;
using P::CatalogTable;
using P::Uuid;

namespace {

// Calls exactly one lambda per alternative. There is no catch-all, so a new
// CatalogItem alternative does not compile until it is handled here.
template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

using Found = Result<std::optional<Uuid>>;

class Finder {
 public:
  explicit Finder(P::IStore& store) : store_(store) {}

  Found named(CatalogTable table, const std::string& name) { return store_.find_catalog_row(table, {name}); }

  Found project(const std::string& name, const std::optional<std::string>& pi_last,
                const std::optional<std::string>& pi_first) {
    P::CatalogKeyPart investigator;  // none
    if (pi_last) {
      auto pi = store_.find_catalog_row(CatalogTable::PrincipalInvestigator, {*pi_last, pi_first.value_or("")});
      if (!pi || !*pi) return pi;
      investigator = **pi;
    }
    return store_.find_catalog_row(CatalogTable::Project, {name, investigator});
  }

  Found sample(const std::string& name, const std::string& project_name, const std::optional<std::string>& pi_last,
               const std::optional<std::string>& pi_first, const std::string& material,
               const std::string& grainsize) {
    auto in = project(project_name, pi_last, pi_first);
    if (!in || !*in) return in;
    auto of = store_.find_catalog_row(CatalogTable::Material, {material, grainsize});
    if (!of || !*of) return of;
    return store_.find_catalog_row(CatalogTable::Sample, {name, **in, **of});
  }

  Found level(const std::string& irradiation, const std::string& name) {
    auto in = named(CatalogTable::Irradiation, irradiation);
    if (!in || !*in) return in;
    return store_.find_catalog_row(CatalogTable::Level, {**in, name});
  }

  Found position(const std::string& irradiation, const std::string& level_name, int hole) {
    auto in = level(irradiation, level_name);
    if (!in || !*in) return in;
    return store_.find_catalog_row(CatalogTable::IrradiationPosition, {**in, hole});
  }

  Found load_position(const LoadPositionItem& item) {
    auto tray = named(CatalogTable::Load, item.load);
    if (!tray || !*tray) return tray;
    auto loaded = store_.find_identifier(item.identifier);
    if (!loaded || !*loaded) return loaded;
    return store_.find_catalog_row(CatalogTable::LoadPosition, {**tray, item.position, **loaded});
  }

  P::IStore& store_;
};

Result<bool> present(const Found& found) {
  if (!found) return fail(found.error());
  return found->has_value();
}

}  // namespace

Result<bool> catalog_row_exists(P::IStore& store, const CatalogItem& item) {
  Finder find(store);
  return std::visit(
      Overloaded{
          [&](const PiItem& i) {
            return present(store.find_catalog_row(CatalogTable::PrincipalInvestigator, {i.last_name, i.first_initial}));
          },
          [&](const ProjectItem& i) { return present(find.project(i.name, i.pi_last_name, i.pi_first_initial)); },
          [&](const MaterialItem& i) {
            return present(store.find_catalog_row(CatalogTable::Material, {i.name, i.grainsize}));
          },
          [&](const SampleItem& i) {
            return present(find.sample(i.fields.name, i.project, i.pi_last_name, i.pi_first_initial, i.material,
                                       i.grainsize));
          },
          [&](const IrradiationItem& i) { return present(find.named(CatalogTable::Irradiation, i.name)); },
          [&](const LevelItem& i) { return present(find.level(i.irradiation, i.name)); },
          [&](const PositionItem& i) -> Result<bool> {
            auto at = present(find.position(i.irradiation, i.level, i.position));
            if (!at || !*at || i.identifier.empty()) return at;
            return present(store.find_identifier(i.identifier));
          },
          [&](const SpecialIdentifierItem& i) { return present(store.find_identifier(i.identifier)); },
          [&](const UserItem& i) { return present(find.named(CatalogTable::User, i.name)); },
          [&](const MassSpecItem& i) { return present(find.named(CatalogTable::MassSpectrometer, i.spec.name)); },
          [&](const ExtractDeviceItem& i) { return present(find.named(CatalogTable::ExtractDevice, i.name)); },
          [&](const LoadItem& i) { return present(find.named(CatalogTable::Load, i.spec.name)); },
          [&](const LoadPositionItem& i) { return present(find.load_position(i)); },
          [&](const RepositoryItem& i) { return present(find.named(CatalogTable::Repository, i.name)); },
          [&](const RefObjectItem& i) {
            return present(
                store.find_catalog_row(CatalogTable::RefObject, {std::string(P::to_string(i.type)), i.key}));
          },
          [&](const InterpretedAgeItem& i) -> Result<bool> {
            return fail(ErrorKind::Protocol,
                        "verify: interpreted age '" + i.key + "' is not a catalog row with a natural key");
          },
      },
      item);
}

}  // namespace pychron::ingest::detail
