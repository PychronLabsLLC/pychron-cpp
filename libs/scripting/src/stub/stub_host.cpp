// The host installed when PYCHRON_SCRIPTING is off: rejects every scripted
// run so a build without Python still links and fails loudly, not silently.

#include "pychron/devices/extraction/capability.hpp"
#include "pychron/scripting/script_host.hpp"

namespace pychron::scripting {
namespace {

Error disabled() {
  return extraction::not_supported("scripting (built without PYCHRON_SCRIPTING)");
}

class StubScriptHost final : public IScriptHost {
 public:
  bool available() const noexcept override { return false; }
  Result<CheckReport> check(const Script&, const ScriptEnvironment&) override { return fail(disabled()); }
  Result<Estimate> estimate(const Script&, const ScriptEnvironment&) override { return fail(disabled()); }
  Result<ScriptResult> run(const Script&, const ScriptEnvironment&, CancelToken&) override {
    return fail(disabled());
  }
  Result<ScriptResult> call_hook(const Script&, std::string_view, const ValueMap&,
                                 const ScriptEnvironment&, CancelToken&) override {
    return fail(disabled());
  }
};

}  // namespace

std::unique_ptr<IScriptHost> make_script_host() { return std::make_unique<StubScriptHost>(); }
bool scripting_enabled() noexcept { return false; }
std::vector<std::string> bound_commands() { return {}; }

}  // namespace pychron::scripting
