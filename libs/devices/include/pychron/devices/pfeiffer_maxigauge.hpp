#pragma once

// Pfeiffer MaxiGauge (TPG 256 A) six-channel gauge controller, config kind
// "pfeiffer_maxigauge". Speaks codec::maxigauge over any Transport (serial,
// TCP, sim).

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/codecs/maxigauge.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// read_pressure() and sample() use the first configured channel; multi-gauge
// managers use IChannelPressureGauge::read_pressure(channel).
class PfeifferMaxiGauge final : public Device,
                                public IPressureGauge,
                                public IChannelPressureGauge,
                                public IScannable {
 public:
  // `channels` must be non-empty, unique and within 1..6 (create() checks).
  // `clock` stamps samples and health; SteadyClock when null.
  PfeifferMaxiGauge(std::string name, Transport& transport, std::vector<int> channels,
                    const Clock* clock = nullptr);

  // Registry hooks (REGISTER_DRIVER).
  static DriverSchema schema();
  static Result<std::unique_ptr<PfeifferMaxiGauge>> create(const DriverArgs& args);

  Result<double> read_pressure() override;
  Result<double> read_pressure(int channel) override;
  std::vector<int> pressure_channels() const override { return channels_; }
  Result<Sample> sample() override;

  // Status and value of all six channels in one query (PRX).
  Result<std::vector<codec::maxigauge::Reading>> read_all();
  // The unit the gauge reports pressures in (UNI).
  Result<codec::maxigauge::Units> read_units();

 private:
  // Mnemonic -> ACK, then ENQ -> data line.
  Result<Bytes> query(const codec::Command& mnemonic);

  Transport& transport_;
  std::vector<int> channels_;
  const Clock* clock_;
  // The gauge answers ENQ with data for the last acknowledged mnemonic, so a
  // query is two exchanges that must not interleave with another query from
  // this driver. The transport keeps each exchange atomic on the bus; this
  // keeps the pair atomic per device.
  std::mutex query_mutex_;
};

// SimSystem hook contract. SimSystem owns the physics and answers
// `pressure(channel)` (channel 1..6, value in `units`, nullopt = no sensor);
// the hook speaks the gauge's side of the wire. Called only from the
// transport worker, so `pressure` need not be reentrant but must be safe to
// call while SimSystem advances its model on another thread.
struct MaxiGaugeSimModel {
  std::function<std::optional<double>(int channel)> pressure;
  codec::maxigauge::Units units = codec::maxigauge::Units::Torr;
};

// For SimTransport::hooked(): ACKs valid mnemonics, NAKs anything else, and
// answers ENQ with data for the last acknowledged mnemonic (NAK if none).
SimTransport::Hook maxigauge_sim_hook(MaxiGaugeSimModel model);

}  // namespace pychron
