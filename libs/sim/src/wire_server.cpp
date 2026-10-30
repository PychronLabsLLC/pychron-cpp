#include "pychron/sim/wire_server.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace pychron::sim {

namespace {

using namespace std::chrono_literals;

// How often a connection looks for unsolicited input (spec section 4.4).
constexpr Duration kPoll = 20ms;
// How long an accept or receive waits before the loop looks at its workers.
constexpr Duration kAcceptSlice = 100ms;
constexpr Duration kWriteTimeout = 5s;
// What a connection keeps of a request that never ends before dropping it.
constexpr std::size_t kMaxPending = 64 * 1024;

}  // namespace

struct WireServer::Served {
  std::string name;
  WireProtocol protocol = WireProtocol::Tcp;
  WireSim sim;
  std::mutex emulator;  // around sim.hook, sim.on_connect, sim.unsolicited
  std::unique_ptr<TcpListener> tcp;
  std::unique_ptr<UdpListener> udp;
};

WireServer::WireServer(Options options) : options_(std::move(options)) {}

WireServer::~WireServer() { stop(); }

Result<void> WireServer::add(std::string transport, WireProtocol protocol, WireSim sim) {
  std::lock_guard lock(mutex_);
  if (started_) return fail(ErrorKind::Config, "sim serve: '" + transport + "' added after the server started");
  if (!sim.request) return fail(ErrorKind::Config, "sim serve: '" + transport + "' has no request framing");
  if (!sim.hook) sim.hook = [](const Bytes&) { return Bytes{}; };
  if (served_.contains(transport)) return fail(ErrorKind::Config, "sim serve: '" + transport + "' added twice");
  auto served = std::make_unique<Served>();
  served->name = transport;
  served->protocol = protocol;
  served->sim = std::move(sim);
  served_.emplace(std::move(transport), std::move(served));
  return {};
}

Result<std::map<std::string, Endpoint>> WireServer::start() {
  if (dynamic_cast<const SteadyClock*>(&clock()) == nullptr) {
    return fail(ErrorKind::Config, "sim serve runs in real time only: its clock must be a SteadyClock");
  }
  {
    std::lock_guard lock(mutex_);
    if (started_ || stopping_) return fail(ErrorKind::Config, "sim serve: the server has already started");
  }

  auto close_all = [this] {
    for (auto& [_, s] : served_) {
      s->tcp.reset();
      s->udp.reset();
    }
  };
  std::map<std::string, Endpoint> endpoints;
  unsigned i = 0;
  for (auto& [name, served] : served_) {
    std::uint16_t port = 0;
    if (options_.port_base != 0) {
      const unsigned wanted = options_.port_base + i;
      if (wanted > 65535) {
        close_all();
        return fail(ErrorKind::Config, "sim serve: '" + name + "' would be on port " + std::to_string(wanted) +
                                           ", past 65535; give a lower --port-base");
      }
      port = static_cast<std::uint16_t>(wanted);
    }
    ++i;
    Error refused;
    if (served->protocol == WireProtocol::Udp) {
      auto bound = UdpListener::bind(options_.bind, port);
      if (bound) {
        served->udp = std::move(*bound);
        port = served->udp->port();
      } else {
        refused = bound.error();
      }
    } else {
      auto bound = TcpListener::bind(options_.bind, port);
      if (bound) {
        served->tcp = std::move(*bound);
        port = served->tcp->port();
      } else {
        refused = bound.error();
      }
    }
    if (!served->tcp && !served->udp) {
      close_all();
      return fail(refused.kind, "sim serve: cannot serve '" + name + "' on port " + std::to_string(port) + ": " +
                                    refused.what);
    }
    endpoints.emplace(name, Endpoint{options_.bind, port, served->protocol, served->sim.emulator});
  }

  {
    std::lock_guard lock(mutex_);
    started_ = true;
  }
  for (auto& [name, served] : served_) {
    Served* s = served.get();
    if (s->tcp) {
      spawn("sim serve " + name, [this, s] { accept_loop(*s); });
    } else {
      spawn("sim serve " + name, [this, s] { datagram_loop(*s); });
    }
  }
  return endpoints;
}

void WireServer::stop() {
  std::list<Worker> joining;
  {
    std::unique_lock lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    for (auto& [_, s] : served_) {
      if (s->tcp) s->tcp->close();
      if (s->udp) s->udp->close();
    }
    for (auto* stream : streams_) stream->close();
    // No worker starts once stopping_ is set: those there are all of them.
    while (!std::ranges::all_of(workers_, [](const Worker& w) { return w.done; })) clock().wait(cv_, lock);
    joining = std::move(workers_);
  }
  for (auto& w : joining) w.thread.join();
  for (auto& [_, s] : served_) {
    s->tcp.reset();
    s->udp.reset();
  }
}

void WireServer::spawn(const std::string& name, std::function<void()> body) {
  // Taken before the thread exists, so that the clock waits for it; the
  // thread drops its copy once it is a participant, this one goes at return.
  auto hold = std::make_shared<Clock::Hold>(clock());
  std::lock_guard lock(mutex_);
  if (stopping_) return;
  Worker& worker = workers_.emplace_back();
  worker.thread = std::thread([this, &worker, hold, name, body = std::move(body)]() mutable {
    {
      const Clock::Participant participant(clock(), name);
      hold.reset();
      body();
      std::lock_guard done(mutex_);
      worker.done = true;
    }
    clock().notify_all(cv_);
  });
}

void WireServer::reap() {
  std::list<Worker> finished;
  {
    std::lock_guard lock(mutex_);
    const auto self = std::this_thread::get_id();
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->done && it->thread.get_id() != self) {
        auto next = std::next(it);
        finished.splice(finished.end(), workers_, it);
        it = next;
      } else {
        ++it;
      }
    }
  }
  for (auto& w : finished) w.thread.join();
}

void WireServer::accept_loop(Served& served) {
  for (;;) {
    auto stream = served.tcp->accept(kAcceptSlice);
    reap();
    if (!stream) {
      if (stream.error().kind == ErrorKind::Timeout) continue;
      return;  // closed by stop(), or the listener failed
    }
    // shared_ptr: std::function wants a copyable callable.
    auto shared = std::make_shared<std::unique_ptr<ByteStream>>(std::move(*stream));
    spawn("sim serve " + served.name + " connection",
          [this, &served, shared] { serve_connection(served, std::move(*shared)); });
  }
}

void WireServer::serve_connection(Served& served, std::unique_ptr<ByteStream> stream) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    streams_.insert(stream.get());
  }
  const WireSim& sim = served.sim;
  if (sim.on_connect) {
    std::lock_guard emulator(served.emulator);
    sim.on_connect();
  }
  Bytes pending;
  bool open = true;
  while (open) {
    auto got = stream->read_some(kPoll);
    if (!got) break;  // the peer closed, or stop() did
    pending.insert(pending.end(), got->begin(), got->end());
    while (open) {
      const auto n = sim.request(pending);
      if (!n || *n == 0) break;
      const Bytes request(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*n));
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*n));
      Bytes reply;
      {
        std::lock_guard emulator(served.emulator);
        reply = sim.hook(request);
      }
      if (!reply.empty() && !stream->write(reply, kWriteTimeout)) open = false;
      if (sim.close_after_reply) open = false;
    }
    if (pending.size() > kMaxPending) pending.clear();
    if (open && sim.unsolicited) {
      Bytes input;
      {
        std::lock_guard emulator(served.emulator);
        input = sim.unsolicited();
      }
      if (!input.empty() && !stream->write(input, kWriteTimeout)) open = false;
    }
  }
  {
    std::lock_guard lock(mutex_);
    streams_.erase(stream.get());
  }
  stream.reset();  // closes the socket: the peer reads its end
}

void WireServer::datagram_loop(Served& served) {
  for (;;) {
    auto datagram = served.udp->receive(kAcceptSlice);
    if (!datagram) {
      if (datagram.error().kind == ErrorKind::Timeout) continue;
      return;
    }
    Bytes reply;
    {
      std::lock_guard emulator(served.emulator);
      reply = served.sim.hook(datagram->data);
    }
    if (!reply.empty()) (void)served.udp->send_to(*datagram->from, reply);
  }
}

}  // namespace pychron::sim
