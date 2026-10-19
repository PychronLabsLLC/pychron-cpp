// elctl db without the DVC store (PYCHRON_PERSISTENCE=OFF, or Qt not found):
// the command exists and says why it does nothing.

#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "db.hpp"

namespace elctl {

int db_command(const std::vector<std::string>&, const std::optional<std::string>&, Io io) {
  io.err << "elctl was built without persistence\n";
  return kUsage;
}

}  // namespace elctl
