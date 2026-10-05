#pragma once

// LineCryoService: the script host's ICryo over the extraction line's
// cryostat ([cryo] in the line config; plan 2026-10-05, task C4). Legacy
// reached the cryostat through the extraction line (CryoManager), so a line
// cryostat serves every run whatever its extract device.
//
//   set_cryo(K)          output 1's setpoint
//   set_cryo_named(n)    [cryo.setpoints].n: value i to output i
//   get_cryo_temp(c)     input c (1 = the first of the controller's inputs),
//                        read now
//   cryo_settling()      true until every output the last set touched reads
//                        within tolerance_k on its paired input (output i
//                        waits on input i, as legacy paired them); an Io
//                        error once timeout_s has passed since that set
//                        (legacy waited forever)
//
// Commands may come from a script thread while the line scans the same
// controller; the transport serialises them. Thread-safe.

#include <map>
#include <mutex>
#include <optional>
#include <string_view>

#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace pychron::systems {

class LineCryoService final : public extraction::ICryo {
 public:
  // `line` must outlive the service and have a [cryo] section.
  explicit LineCryoService(ExtractionLine& line);

  Result<void> set_cryo(double setpoint_kelvin) override;
  Result<double> get_cryo_temp(int channel) override;
  Result<void> set_cryo_named(std::string_view name) override;
  Result<bool> cryo_settling() override;

 private:
  Result<ITemperatureController*> controller() const;
  Result<void> apply(const std::map<int, double>& setpoints);

  ExtractionLine& line_;
  mutable std::mutex mutex_;
  std::map<int, double> targets_;  // output -> kelvin, of the last set
  TimePoint set_at_{};
};

}  // namespace pychron::systems
