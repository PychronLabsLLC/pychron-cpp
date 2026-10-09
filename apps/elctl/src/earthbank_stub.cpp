// elctl earthbank without the DVC store (PYCHRON_PERSISTENCE=OFF, or Qt not
// found): the command exists and says why it does nothing. The commands that
// would need the network say that first when this build has none either.

#include <ostream>
#include <string>

#include "earthbank.hpp"

namespace elctl {

int earthbank_command(const std::vector<std::string>& args, Io io, EarthbankDeps) {
#ifndef PYCHRON_ELCTL_HAS_NETWORK
  if (!args.empty() && (args[0] == "login" || args[0] == "account" || args[0] == "packages" || args[0] == "preflight" ||
                        args[0] == "upload")) {
    io.err << "this build has no network support (Qt6 Network was not found)\n";
    return kUsage;
  }
#else
  (void)args;
#endif
  io.err << "elctl was built without persistence\n";
  return kUsage;
}

}  // namespace elctl
