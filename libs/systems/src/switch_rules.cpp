#include "pychron/systems/switch_rules.hpp"

namespace pychron::systems {

Result<void> check_switch_access(const SwitchView& self, std::string_view actor) {
  if (self.locked) return fail(ErrorKind::Interlock, "'" + self.name + "' is software locked", self.name);
  if (!self.owner.empty() && self.owner != actor) {
    return fail(ErrorKind::Interlock,
                "'" + self.name + "' is owned by '" + self.owner + "', not '" + std::string(actor) + "'",
                self.name);
  }
  return {};
}

Result<void> check_switch_interlocks(const SwitchView& self, const std::vector<const SwitchView*>& exclusive,
                                     const std::vector<const SwitchView*>& positive) {
  for (const SwitchView* o : exclusive) {
    if (o->state != ValveState::Closed) {
      return fail(ErrorKind::Interlock,
                  "cannot open '" + self.name + "': interlocked with '" + o->name + "' which is " +
                      std::string(pychron::to_string(o->state)),
                  self.name);
    }
  }
  for (const SwitchView* p : positive) {
    if (p->state != ValveState::Open) {
      return fail(ErrorKind::Interlock,
                  "cannot open '" + self.name + "': requires '" + p->name + "' open, it is " +
                      std::string(pychron::to_string(p->state)),
                  self.name);
    }
  }
  return {};
}

}  // namespace pychron::systems
