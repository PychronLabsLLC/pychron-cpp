#include "pychron/entry/holder_import.hpp"

#include <set>

#include "pychron/dvc/meta_files.hpp"

namespace pychron::entry {

namespace ps = persistence;

Result<ps::HolderValue> read_holder(std::string_view text) {
  auto holder = dvc::parse_holder_text(text);
  if (!holder) return fail(holder.error());
  if (holder->holes.empty()) return fail(ErrorKind::Config, "the holder has no holes");
  std::set<std::string> ids;
  for (const auto& h : holder->holes)
    if (!ids.insert(h.hole_id).second) return fail(ErrorKind::Config, "hole " + h.hole_id + " is listed twice");
  return std::move(*holder);
}

Result<ps::Uuid> save_holder(ps::IStore& store, const ps::Actor& actor, const std::string& name,
                             const ps::HolderValue& holder) {
  ps::RefObjectSpec spec;
  spec.type = ps::RefType::IrradiationHolder;
  spec.key = name;
  auto object = store.add_ref_object(actor.client, spec);
  if (!object) return fail(object.error());
  auto head = store.head(*object, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  if (auto r = (*uow)->add_revision(*object, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{holder}}, *head); !r)
    return fail(r.error());
  auto out = (*uow)->commit(ps::ChangesetKind::Reference, "holder " + name);
  if (!out) return fail(out.error());
  if (!std::holds_alternative<ps::Committed>(*out))
    return fail(ErrorKind::Config, "holder " + name + " was changed by another client; try again");
  return *object;
}

Result<std::optional<ps::HolderValue>> load_holder(ps::IStore& store, ps::Uuid holder) {
  auto head = store.head(holder, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  if (!*head) return std::optional<ps::HolderValue>{};
  auto payload = store.load_payload(**head);
  if (!payload) return fail(payload.error());
  if (!*payload) return std::optional<ps::HolderValue>{};
  const auto* ref = std::get_if<ps::RefPayload>(&**payload);
  const auto* value = ref ? std::get_if<ps::HolderValue>(ref) : nullptr;
  if (!value) return fail(ErrorKind::Protocol, "not a holder");
  return std::optional<ps::HolderValue>{*value};
}

}  // namespace pychron::entry
