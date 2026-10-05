#pragma once

// A legacy Pychron valve service (base_valve.py) for SimSystem and driver
// tests: Open/Close answer OK (ok when nothing changed), GetValveState OK or
// False, an unknown valve ERROR 005, a locked one ERROR 014, anything else
// ERROR 003. With no valves declared it serves any name. Every valve starts
// closed. The listener fires on the transport's worker thread for each
// Open/Close that changed a valve.

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "pychron/transport/sim_transport.hpp"

namespace pychron {

class PychronValveServerSim {
 public:
  using Listener = std::function<void(const std::string& name, bool open)>;

  explicit PychronValveServerSim(Listener on_change = {});
  PychronValveServerSim(const PychronValveServerSim&) = delete;
  PychronValveServerSim& operator=(const PychronValveServerSim&) = delete;

  Bytes respond(const Bytes& tx);
  SimTransport::Hook hook();  // the server must outlive the transport

  // Serve only these names from now on.
  void declare(std::set<std::string> names);
  void lock(const std::string& name);
  bool is_open(const std::string& name) const;

 private:
  Listener on_change_;
  mutable std::mutex mutex_;
  std::set<std::string> declared_;
  std::set<std::string> locked_;
  std::map<std::string, bool> open_;
};

}  // namespace pychron
