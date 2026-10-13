// EarthBank over the network: IHttpClient on Qt Network.
//
// Blocking, as the seam asks (http.hpp): `send` returns when the answer is in
// or when it is known that none will be, from any thread. It runs a nested
// Qt event loop for the exchange; a program with no QCoreApplication gets one
// of its own at the first `send` (and keeps it for the life of the process),
// so a command-line program or a worker thread needs nothing set up. A GUI
// program calls it from a worker thread, not from the thread that paints.
//
// What it promises beyond the seam:
//   - HTTPS is verified against the system's store of certificate
//     authorities, and an error in a certificate is a failure to connect
//     (`Io`, kHttpNotSent): none is ever ignored. `http://` is spoken too,
//     which is for a test server on this machine; which addresses a program
//     may be given is its profiles' business (profiles.hpp).
//   - Nothing is sent twice and nothing is followed. Each request has a
//     connection of its own, so there is no kept-open connection to find
//     closed and be retried on, and a 3xx is the answer it is, whatever host
//     its Location names.
//   - The request goes with `User-Agent: pychron-cpp/<version>` unless the
//     request has a User-Agent of its own.
//   - An empty body is allowed, in a request and in an answer.
//   - A transport failure is an `Io` error (`Timeout` when the time ran out)
//     with Qt's description of it, and neither the URL's query, a header nor
//     the body in the message.
#pragma once

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"

namespace pychron::earthbank {

class QtHttpClient final : public IHttpClient {
 public:
  QtHttpClient() = default;
  Result<HttpResponse> send(const HttpRequest& request) override;
};

}  // namespace pychron::earthbank
