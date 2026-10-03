#include "catalog.hpp"

#include <utility>

#include "pychron/ingest/ids.hpp"

namespace pychron::ingest::detail {

namespace P = pychron::persistence;
using P::Uuid;

namespace {

// Calls exactly one lambda per alternative. There is no catch-all, so a new
// CatalogItem alternative does not compile until write() handles it.
template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

std::string join(std::initializer_list<std::string_view> parts) {
  std::string out;
  bool first = true;
  for (auto part : parts) {
    if (!first) out += '\n';
    out += part;
    first = false;
  }
  return out;
}

Result<void> done(const Result<Uuid>& r) {
  if (!r) return fail(r.error());
  return {};
}

}  // namespace

template <class Ensure>
Result<Uuid> CatalogResolver::cached(const char* table, const std::string& key, Ensure&& ensure) {
  std::string cache_key = join({table, key});
  if (auto it = known_.find(cache_key); it != known_.end()) return it->second;
  Result<Uuid> uuid = ensure(catalog_id(table, key));
  if (uuid) known_.emplace(std::move(cache_key), *uuid);
  return uuid;
}

Result<void> CatalogResolver::write(const CatalogItem& item) {
  return std::visit(
      Overloaded{
          [&](const PiItem& i) { return done(principal_investigator(i)); },
          [&](const ProjectItem& i) { return done(project({i.name, i.pi_last_name, i.pi_first_initial}, &i)); },
          [&](const MaterialItem& i) { return done(material(i.name, i.grainsize)); },
          [&](const SampleItem& i) {
            return done(sample(i.fields, {i.project, i.pi_last_name, i.pi_first_initial}, i.material, i.grainsize));
          },
          [&](const IrradiationItem& i) { return done(irradiation(i.name, i.created)); },
          [&](const LevelItem& i) { return done(level(i)); },
          [&](const PositionItem& i) { return done(position(i)); },
          [&](const SpecialIdentifierItem& i) {
            return done(cached("identifier", i.identifier, [&](Uuid id) -> Result<Uuid> {
              P::IdentifierSpec spec;
              spec.identifier = i.identifier;
              spec.kind = "special";
              spec.analysis_type = i.analysis_type;
              if (i.mass_spectrometer) {
                auto ms = mass_spectrometer({*i.mass_spectrometer, std::nullopt, std::nullopt, std::nullopt});
                if (!ms) return fail(ms.error());
                spec.mass_spectrometer = *ms;
              }
              spec.uuid = id;
              return store_.add_identifier(client_, spec);
            }));
          },
          [&](const UserItem& i) { return done(user(i)); },
          [&](const MassSpecItem& i) { return done(mass_spectrometer(i.spec)); },
          [&](const ExtractDeviceItem& i) {
            return done(cached("extract_device", i.name,
                               [&](Uuid) { return store_.add_extract_device(client_, i.name); }));
          },
          [&](const LoadItem& i) { return done(load(i)); },
          [&](const LoadPositionItem& i) -> Result<void> {
            LoadItem bare;
            bare.spec.name = i.load;
            auto tray = load(bare);
            if (!tray) return fail(tray.error());
            auto loaded = identifier(i.identifier);
            if (!loaded) return fail(loaded.error());
            return store_.add_load_position(client_, {*tray, i.position, *loaded, i.weight, i.nxtals, i.note});
          },
          [&](const RepositoryItem& i) { return done(repository(i.name)); },
          [&](const RefObjectItem& i) { return done(ref_object(i.type, i.key, &i)); },
          [&](const InterpretedAgeItem& i) { return done(interpreted_age(i)); },
      },
      item);
}

Result<Uuid> CatalogResolver::user(const std::string& name) {
  return cached("app_user", name, [&](Uuid) { return store_.ensure_user(client_, name); });
}

Result<Uuid> CatalogResolver::user(const UserItem& item) {
  return cached("app_user", item.name, [&](Uuid) {
    return store_.add_user(client_, {item.name, item.email, item.affiliation, item.category});
  });
}

Result<Uuid> CatalogResolver::load(const LoadItem& item) {
  return cached("load", item.spec.name, [&](Uuid id) -> Result<Uuid> {
    P::LoadSpec spec = item.spec;
    spec.holder.reset();
    if (item.holder_name) {
      auto holder = ref_object(P::RefType::LoadHolder, *item.holder_name, nullptr);
      if (!holder) return fail(holder.error());
      spec.holder = *holder;
    }
    if (item.created_by) {
      auto creator = user(*item.created_by);
      if (!creator) return fail(creator.error());
      spec.created_by_user = *creator;
    }
    spec.uuid = id;
    return store_.add_load(client_, spec);
  });
}

// An identifier named by something loaded: the one a PositionItem or
// SpecialIdentifierItem made, else a bare unknown.
Result<Uuid> CatalogResolver::identifier(const std::string& name) {
  return cached("identifier", name, [&](Uuid id) {
    P::IdentifierSpec spec;
    spec.identifier = name;
    spec.uuid = id;
    return store_.add_identifier(client_, spec);
  });
}

Result<Uuid> CatalogResolver::repository(const std::string& name) {
  return cached("repository", name, [&](Uuid) { return store_.add_repository(client_, name); });
}

Result<Uuid> CatalogResolver::ref_object(const RefObjectKey& key) {
  const auto type = P::parse_ref_type(key.ref_type);
  if (!type) return fail(ErrorKind::Protocol, "unknown reference object type '" + key.ref_type + "'");
  return ref_object(*type, key.name, nullptr);
}

Result<Uuid> CatalogResolver::ref_object_id(const RefObjectKey& key) const {
  const auto type = P::parse_ref_type(key.ref_type);
  if (!type) return fail(ErrorKind::Protocol, "unknown reference object type '" + key.ref_type + "'");
  const std::string natural = join({P::to_string(*type), key.name});
  if (auto it = known_.find(join({"ref_object", natural})); it != known_.end()) return it->second;
  return catalog_id("ref_object", natural);
}

Result<Uuid> CatalogResolver::interpreted_age(const InterpretedAgeKey& key) {
  return interpreted_age(InterpretedAgeItem{key.name, key.name, std::nullopt, std::nullopt});
}

// The store cannot look an identifier up, and add_identifier would create a
// bare one. An identifier is therefore linked only when an analysis uses it:
// then it exists, and add_identifier returns it without writing.
Result<Uuid> CatalogResolver::interpreted_age(const InterpretedAgeItem& item) {
  return cached("interpreted_age", join({url_, item.key}), [&](Uuid) -> Result<Uuid> {
    P::InterpretedAgeSpec spec;
    spec.name = item.name;
    if (item.identifier) {
      P::AnalysisQuery query;
      query.identifier = *item.identifier;
      query.limit = 1;
      auto used = store_.find_analyses(query);
      if (!used) return fail(used.error());
      if (!used->empty()) {
        P::IdentifierSpec identifier;
        identifier.identifier = *item.identifier;
        auto found = store_.add_identifier(client_, identifier);
        if (!found) return fail(found.error());
        spec.identifier = *found;
      }
    }
    if (item.repository) {
      auto repo = repository(*item.repository);
      if (!repo) return fail(repo.error());
      spec.repository = *repo;
    }
    spec.uuid = interpreted_age_id(url_, item.key);
    return store_.add_interpreted_age(client_, spec);
  });
}

Result<Uuid> CatalogResolver::principal_investigator(const PiItem& item) {
  return cached("principal_investigator", join({item.last_name, item.first_initial}), [&](Uuid id) {
    return store_.add_principal_investigator(client_,
                                             {item.last_name, item.first_initial, item.affiliation, item.email, id});
  });
}

Result<Uuid> CatalogResolver::project(const ProjectKey& key, const ProjectItem* full) {
  const std::string last = key.pi_last_name.value_or("");
  const std::string first = key.pi_first_initial.value_or("");
  return cached("project", join({key.name, last, first}), [&](Uuid id) -> Result<Uuid> {
    std::optional<Uuid> pi;
    if (key.pi_last_name) {
      auto found = principal_investigator({last, first, std::nullopt, std::nullopt});
      if (!found) return fail(found.error());
      pi = *found;
    }
    P::ProjectSpec spec{key.name, pi, id, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    if (full) {
      spec.checkin_date = full->checkin_date;
      spec.comment = full->comment;
      spec.lab_contact = full->lab_contact;
      spec.institution = full->institution;
    }
    return store_.add_project(client_, spec);
  });
}

Result<Uuid> CatalogResolver::material(const std::string& name, const std::string& grainsize) {
  return cached("material", join({name, grainsize}),
                [&](Uuid id) { return store_.add_material(client_, {name, grainsize, id}); });
}

Result<Uuid> CatalogResolver::sample(P::SampleSpec fields, const ProjectKey& project_key, const std::string& material_name,
                                     const std::string& grainsize) {
  const std::string key = join({fields.name, project_key.name, project_key.pi_last_name.value_or(""),
                                project_key.pi_first_initial.value_or(""), material_name, grainsize});
  return cached("sample", key, [&](Uuid id) -> Result<Uuid> {
    auto p = project(project_key);
    if (!p) return fail(p.error());
    auto m = material(material_name, grainsize);
    if (!m) return fail(m.error());
    fields.project = *p;
    fields.material = *m;
    fields.uuid = id;
    return store_.add_sample(client_, fields);
  });
}

Result<Uuid> CatalogResolver::irradiation(const std::string& name, std::optional<P::UtcTime> created) {
  return cached("irradiation", name,
                [&](Uuid) { return store_.add_irradiation(client_, P::IrradiationSpec{name, created}); });
}

Result<Uuid> CatalogResolver::level(const LevelItem& item) {
  return cached("level", join({item.irradiation, item.name}), [&](Uuid id) -> Result<Uuid> {
    auto irrad = irradiation(item.irradiation);
    if (!irrad) return fail(irrad.error());
    P::LevelSpec spec;
    spec.irradiation = *irrad;
    spec.name = item.name;
    if (item.holder) {
      auto holder = ref_object(P::RefType::IrradiationHolder, *item.holder, nullptr);
      if (!holder) return fail(holder.error());
      spec.holder = *holder;
    }
    spec.z = item.z;
    spec.note = item.note;
    spec.uuid = id;
    return store_.add_level(client_, spec);
  });
}

Result<Uuid> CatalogResolver::position(const PositionItem& item) {
  const std::string key = join({item.irradiation, item.level, std::to_string(item.position)});
  auto position = cached("irradiation_position", key, [&](Uuid id) -> Result<Uuid> {
    auto lvl = level({item.irradiation, item.level, std::nullopt, std::nullopt, std::nullopt});
    if (!lvl) return fail(lvl.error());
    P::PositionSpec spec;
    spec.level = *lvl;
    spec.position = item.position;
    spec.weight = item.weight;
    spec.packet = item.packet;
    spec.note = item.note;
    if (item.sample) {
      if (!item.project || !item.material)
        return fail(ErrorKind::Protocol, "irradiation position " + item.irradiation + "/" + item.level + "/" +
                                             std::to_string(item.position) + ": sample '" + *item.sample +
                                             "' needs a project and a material");
      P::SampleSpec fields;
      fields.name = *item.sample;
      auto s = sample(std::move(fields), {*item.project, item.pi_last_name, item.pi_first_initial}, *item.material,
                      item.grainsize.value_or(""));
      if (!s) return fail(s.error());
      spec.sample = *s;
    }
    spec.uuid = id;
    return store_.add_irradiation_position(client_, spec);
  });
  if (!position || item.identifier.empty()) return position;

  auto identifier = cached("identifier", item.identifier, [&](Uuid id) {
    P::IdentifierSpec spec;
    spec.identifier = item.identifier;
    spec.kind = "unknown";
    spec.position = *position;
    spec.uuid = id;
    return store_.add_identifier(client_, spec);
  });
  if (!identifier) return fail(identifier.error());
  return position;
}

Result<Uuid> CatalogResolver::mass_spectrometer(P::MassSpectrometerSpec spec) {
  return cached("mass_spectrometer", spec.name, [&](Uuid id) {
    spec.uuid = id;
    return store_.add_mass_spectrometer(client_, spec);
  });
}

// `scope`: the item that names the object's scope; nullptr creates it unscoped.
Result<Uuid> CatalogResolver::ref_object(P::RefType type, const std::string& key, const RefObjectItem* scope) {
  return cached("ref_object", join({P::to_string(type), key}), [&](Uuid id) -> Result<Uuid> {
    P::RefObjectSpec spec;
    spec.type = type;
    spec.key = key;
    spec.uuid = id;
    if (scope && scope->irradiation) {
      auto irrad = irradiation(*scope->irradiation);
      if (!irrad) return fail(irrad.error());
      spec.irradiation = *irrad;
      if (scope->level) {
        auto lvl = level({*scope->irradiation, *scope->level, std::nullopt, std::nullopt, std::nullopt});
        if (!lvl) return fail(lvl.error());
        spec.level = *lvl;
        if (scope->position) {
          PositionItem at;
          at.irradiation = *scope->irradiation;
          at.level = *scope->level;
          at.position = *scope->position;
          auto pos = position(at);
          if (!pos) return fail(pos.error());
          spec.position = *pos;
        }
      }
    }
    if (scope && scope->mass_spectrometer) {
      auto ms = mass_spectrometer({*scope->mass_spectrometer, std::nullopt, std::nullopt, std::nullopt});
      if (!ms) return fail(ms.error());
      spec.mass_spectrometer = *ms;
    }
    return store_.add_ref_object(client_, spec);
  });
}

}  // namespace pychron::ingest::detail
