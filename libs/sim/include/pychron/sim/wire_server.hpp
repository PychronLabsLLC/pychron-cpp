#pragma once

// WireServer: emulators behind TCP and UDP listeners, one listener per
// transport (sim serve spec, docs/superpowers/specs/2026-10-09-sim-serve-design.md
// section 4.4). A driver on a real TcpTransport or UdpTransport talks to it
// as it would to the device.
//
//   TCP  each connection has a thread: it cuts requests from the stream with
//        WireSim::request, calls the hook for each and writes back a reply
//        that is not empty, closing the connection after it when
//        close_after_reply; every 20 ms it polls unsolicited input and writes
//        what there is. Any number of connections, all on one emulator.
//   UDP  one datagram is one request; the reply goes back to its sender.
//
// The emulators were written for one caller, so the hook, on_connect and
// unsolicited of one transport are called under one mutex, whichever
// connection calls; it is never held across a socket wait.
//
// Real time only: sockets wait in real time, and a simulated clock would
// stand still behind them. start() refuses any clock but a SteadyClock. The
// threads follow the time rules all the same (Participant, Hold, a joined
// thread says it is done through the clock), which on a SteadyClock cost
// nothing.

#include <condition_variable>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/sim/wire_sim.hpp"
#include "pychron/transport/endpoints.hpp"
#include "pychron/transport/listener.hpp"

namespace pychron::sim {

class WireServer {
 public:
  struct Options {
    std::string bind = "127.0.0.1";
    // 0: each listener on a port the OS chooses. Else port_base + i, i
    // counting the transports added in byte order of their names.
    std::uint16_t port_base = 0;
    const Clock* clock = nullptr;  // a SteadyClock; null: one of the server's own
  };

  explicit WireServer(Options options);
  ~WireServer();
  WireServer(const WireServer&) = delete;
  WireServer& operator=(const WireServer&) = delete;

  // Config error for a name added twice, an empty request framer, or once started.
  Result<void> add(std::string transport, WireProtocol protocol, WireSim sim);
  // Binds every listener, then serves. All or nothing: on a failure to bind
  // every listener is closed again and the error names the transport and
  // the port. Config for a clock that is not a SteadyClock ("sim serve runs
  // in real time only") or a port past 65535. By transport name; each
  // endpoint's host is Options::bind and its emulator the WireSim's.
  Result<std::map<std::string, Endpoint>> start();
  // Closes every listener and connection and joins every thread. Idempotent.
  void stop();

 private:
  struct Served;
  struct Worker {
    std::thread thread;
    bool done = false;  // under mutex_
  };

  const Clock& clock() const noexcept {
    if (options_.clock != nullptr) return *options_.clock;
    return own_clock_;
  }
  // Starts `body` on a participant thread, unless stopping. Takes mutex_.
  void spawn(const std::string& name, std::function<void()> body);
  // Joins the workers that have said they are done, but the calling thread's.
  void reap();
  void accept_loop(Served& served);
  void serve_connection(Served& served, std::unique_ptr<ByteStream> stream);
  void datagram_loop(Served& served);

  Options options_;
  const SteadyClock own_clock_;
  std::map<std::string, std::unique_ptr<Served>> served_;  // by name: the byte order of ports

  std::mutex mutex_;  // guards what follows
  std::condition_variable cv_;  // a worker is done; waited on through the clock
  bool started_ = false;
  bool stopping_ = false;
  std::list<Worker> workers_;
  std::set<ByteStream*> streams_;  // open connections, closed by stop()
};

}  // namespace pychron::sim
