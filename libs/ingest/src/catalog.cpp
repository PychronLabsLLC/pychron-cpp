#include "catalog.hpp"

#include <cstdio>
#include <string_view>
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

// The optional values a call brings, as text: "" when it brings none.
class Values {
 public:
  Values& operator()(std::string_view name, const std::optional<std::string>& value) {
    if (value) add(name, *value);
    return *this;
  }
  Values& operator()(std::string_view name, const std::optional<double>& value) {
    if (value) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", *value);
      add(name, buf);
    }
    return *this;
  }
  Values& operator()(std::string_view name, const std::optional<Uuid>& value) {
    if (value) add(name, value->str());
    return *this;
  }
  const std::string& text() const { return text_; }

 private:
  void add(std::string_view name, std::string_view value) {
    text_ += name;
    text_ += '=';
    text_ += value;
    text_ += '\n';
  }
  std::string text_;
};

}  // namespace

template <class Ensure>
Result<Uuid> CatalogResolver::cached(const char* table, const std::string& key, const std::string& values,
                                     Ensure&& ensure) {
  const std::string cache_key = join({table, key});
  const std::string sent_key = cache_key + "\n\n" + values;
  if (auto it = known_.find(cache_key); it != known_.end() && (values.empty() || sent_.contains(sent_key)))
    return it->second;
  Result<Uuid> uuid = ensure(catalog_id(table, key));
  if (!uuid) {
    // The row a fill was refused for is the innermost one: a row that names
    // it fails with the same error and is not it.
    if (!refused_ && P::is_refused_catalog_fill(uuid.error())) {
      RefusedFill refused{table, {}, uuid.error().what};
      for (std::size_t from = 0;;) {
        const auto end = key.find('\n', from);
        refused.natural_key.push_back(key.substr(from, end == std::string::npos ? end : end - from));
        if (end == std::string::npos) break;
        from = end + 1;
      }
      refused_ = std::move(refused);
    }
    return uuid;
  }
  known_.insert_or_assign(cache_key, *uuid);
  if (!values.empty()) sent_.insert(sent_key);
  return uuid;
}

Result<void> CatalogResolver::write(const CatalogItem& item) {
  refused_.reset();
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
            const std::string values =
                Values()("analysis_type", std::optional<std::string>{i.analysis_type})("ms", i.mass_spectrometer).text();
            return done(cached("identifier", i.identifier, values, [&](Uuid id) -> Result<Uuid> {
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
            return done(cached("extract_device", i.name, "",
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
  return cached("app_user", name, "", [&](Uuid) { return store_.ensure_user(client_, name); });
}

Result<Uuid> CatalogResolver::user(const UserItem& item) {
  const std::string values =
      Values()("email", item.email)("affiliation", item.affiliation)("category", item.category).text();
  return cached("app_user", item.name, values, [&](Uuid) {
    return store_.add_user(client_, {item.name, item.email, item.affiliation, item.category});
  });
}

Result<Uuid> CatalogResolver::load(const LoadItem& item) {
  const std::string values = Values()("holder", item.holder_name)("holder_revision", item.spec.holder_revision)(
      "created_by", item.created_by)("created_by_user", item.spec.created_by_user)
                                 .text();
  return cached("load", item.spec.name, values, [&](Uuid id) -> Result<Uuid> {
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
  return cached("identifier", name, "", [&](Uuid id) {
    P::IdentifierSpec spec;
    spec.identifier = name;
    spec.uuid = id;
    return store_.add_identifier(client_, spec);
  });
}

Result<Uuid> CatalogResolver::repository(const std::string& name) {
  return cached("repository", name, "", [&](Uuid) { return store_.add_repository(client_, name); });
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
  return cached("interpreted_age", join({url_, item.key}), "", [&](Uuid) -> Result<Uuid> {
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
  const std::string values = Values()("affiliation", item.affiliation)("email", item.email).text();
  return cached("principal_investigator", join({item.last_name, item.first_initial}), values, [&](Uuid id) {
    return store_.add_principal_investigator(client_,
                                             {item.last_name, item.first_initial, item.affiliation, item.email, id});
  });
}

Result<Uuid> CatalogResolver::project(const ProjectKey& key, const ProjectItem* full) {
  const std::string last = key.pi_last_name.value_or("");
  const std::string first = key.pi_first_initial.value_or("");
  Values values;
  if (full)
    values("checkin_date", full->checkin_date)("comment", full->comment)("lab_contact", full->lab_contact)(
        "institution", full->institution);
  return cached("project", join({key.name, last, first}), values.text(), [&](Uuid id) -> Result<Uuid> {
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
  return cached("material", join({name, grainsize}), "",
                [&](Uuid id) { return store_.add_material(client_, {name, grainsize, id}); });
}

Result<Uuid> CatalogResolver::sample(P::SampleSpec fields, const ProjectKey& project_key, const std::string& material_name,
                                     const std::string& grainsize) {
  const std::string key = join({fields.name, project_key.name, project_key.pi_last_name.value_or(""),
                                project_key.pi_first_initial.value_or(""), material_name, grainsize});
  const std::string values =
      Values()("note", fields.note)("igsn", fields.igsn)("lat", fields.lat)("lon", fields.lon)(
          "elevation", fields.elevation)("storage_location", fields.storage_location)("location", fields.location)(
          "unit", fields.unit)("lithology", fields.lithology)("lithology_class", fields.lithology_class)(
          "lithology_type", fields.lithology_type)("lithology_group", fields.lithology_group)(
          "approximate_age", fields.approximate_age)
          .text();
  return cached("sample", key, values, [&](Uuid id) -> Result<Uuid> {
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
  // The time an irradiation was made is a value it always has: nothing to fill.
  return cached("irradiation", name, "",
                [&](Uuid) { return store_.add_irradiation(client_, P::IrradiationSpec{name, created}); });
}

Result<Uuid> CatalogResolver::level(const LevelItem& item) {
  const std::string values = Values()("holder", item.holder)("z", item.z)("note", item.note).text();
  return cached("level", join({item.irradiation, item.name}), values, [&](Uuid id) -> Result<Uuid> {
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
  Values values;
  values("weight", item.weight)("packet", item.packet)("note", item.note);
  if (item.sample)
    values("sample", item.sample)("project", item.project)("material", item.material)("grainsize", item.grainsize)(
        "pi_last_name", item.pi_last_name)("pi_first_initial", item.pi_first_initial);
  auto position = cached("irradiation_position", key, values.text(), [&](Uuid id) -> Result<Uuid> {
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

  const std::string at = Values()("position", std::optional<Uuid>{*position}).text();
  auto identifier = cached("identifier", item.identifier, at, [&](Uuid id) {
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
  const std::string values = Values()("kind", spec.kind)("code", spec.code).text();
  return cached("mass_spectrometer", spec.name, values, [&](Uuid id) {
    spec.uuid = id;
    return store_.add_mass_spectrometer(client_, spec);
  });
}

// `scope`: the item that names the object's scope; nullptr creates it unscoped.
Result<Uuid> CatalogResolver::ref_object(P::RefType type, const std::string& key, const RefObjectItem* scope) {
  Values values;
  if (scope) {
    values("irradiation", scope->irradiation)("level", scope->level)("ms", scope->mass_spectrometer);
    if (scope->position) values("position", std::optional<std::string>{std::to_string(*scope->position)});
  }
  return cached("ref_object", join({P::to_string(type), key}), values.text(), [&](Uuid id) -> Result<Uuid> {
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
