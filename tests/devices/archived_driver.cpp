// Compiled into a separate static library that nothing references by symbol.
// Its REGISTER_DRIVER must still run: see pychron_link_drivers().

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/driver_registry.hpp"

namespace {

class ArchivedDriver : public pychron::Device, public pychron::IScannable {
 public:
  explicit ArchivedDriver(std::string name) : Device(std::move(name)) {}

  static pychron::DriverSchema schema() { return {"", "linker fixture", {}}; }
  static pychron::Result<std::unique_ptr<ArchivedDriver>> create(const pychron::DriverArgs& args) {
    return std::make_unique<ArchivedDriver>(args.name);
  }

  pychron::Result<pychron::Sample> sample() override { return pychron::Sample{name(), {}, 0.0}; }
};

}  // namespace

REGISTER_DRIVER("test_archived_driver", ArchivedDriver);
