#pragma once

// Stateful model of an Agilent 34970A with switch cards, for SimSystem and
// driver tests. Answers as codec::agilent describes: *IDN?, *CLS,
// SYST:ERR?, ROUT:OPEN / ROUT:CLOSE (no reply) and their queries, for one
// channel at a time. A command it does not know, or a channel that is not
// fitted, queues an instrument error and gets no reply, as the unit does.
// Relays start closed, or open with `relays_start_closed` false: SimSystem
// picks whichever makes every valve start closed for the unit's `invert`,
// as sim_valves does (a real unit powers up however its relays were left).
// The listener fires on the transport's worker thread for each route
// command, with the channel and whether its relay is now closed.

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "pychron/codecs/agilent.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron {

class AgilentUnitSim {
 public:
  using Listener = std::function<void(const std::string& channel, bool relay_closed)>;

  // Fitted channels: slots 1..3, channels 01..20 (34903A cards).
  explicit AgilentUnitSim(Listener on_route = {}, bool relays_start_closed = true);
  AgilentUnitSim(const AgilentUnitSim&) = delete;
  AgilentUnitSim& operator=(const AgilentUnitSim&) = delete;

  Bytes respond(const Bytes& tx);
  // respond() as a SimTransport hook. The unit must outlive the transport.
  SimTransport::Hook hook();

  bool relay_closed(const std::string& channel) const;
  std::size_t queued_errors() const;
  // The identity *IDN? reports; default "Agilent Technologies,34970A,...".
  void set_identity(std::string line);
  // Queue an instrument error, as a fault would.
  void push_error(int code, std::string message);

 private:
  void error_locked(int code, std::string message);

  Listener on_route_;
  mutable std::mutex mutex_;
  std::string identity_ = "Agilent Technologies,34970A,MY00000000,13-2-2";
  std::set<std::string> fitted_;
  std::map<std::string, bool> closed_;  // by fitted channel
  std::deque<codec::agilent::InstrumentError> errors_;
};

}  // namespace pychron
