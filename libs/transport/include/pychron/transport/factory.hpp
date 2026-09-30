#pragma once

#include <memory>
#include <string>

#include "pychron/core/config/system_config.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

struct TransportContext {
  const Clock* clock = nullptr;    // health timestamps and trace times; SteadyClock if null
  SignalBus* bus = nullptr;        // TransportHealth events
  SimTransport::Hook sim_hook;     // reply source for kind = "sim"; silent wire if empty
  std::string trace_dir = ".";     // `<trace_dir>/<name>.trace` when config.trace is set
};

// Builds the transport a [transports.<name>] entry describes, closed.
// kind = "sim" yields a hooked SimTransport, so the same config runs with no
// hardware. When `trace` is set the result is wrapped in a TraceRecorder.
// Config error for kinds not yet supported (modbus_rtu, modbus_tcp).
Result<std::unique_ptr<Transport>> make_transport(const config::TransportConfig& config,
                                                  const TransportContext& context = {});

}  // namespace pychron
