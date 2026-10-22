// The seam between a library and the network.
//
// A library that talks to a web service says what to ask and what to make of
// the answer; it does not open a socket. Everything it sends goes through
// IHttpClient, which a program gives it (the one over HTTPS lives in
// `libs/earthbank_qt`), and a test gives it a fake. Nothing here does I/O.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::http {

// One request: the method in capitals (`GET`, `POST`, `DELETE`), the whole
// URL with its query already encoded, the headers as they go on the wire, and
// the body as bytes. `timeout` is for the whole exchange.
struct HttpRequest {
  std::string method, url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::chrono::milliseconds timeout{30000};
};

// What the server said. Any status is an answer, 401 and 500 included.
struct HttpResponse {
  int status = 0;
  std::string body;
};

// The `code` of a `Timeout` or `Io` error from `send` when the request never
// left. What such a request asked for was not done.
inline constexpr std::string_view kHttpNotSent = "http_not_sent";

// Blocking: `send` returns when the answer is in, or when it is known that
// none will be. It is called from one thread at a time.
//
// One call is one request on the wire, at most. An implementation never
// sends a request a second time on its own: not after a timeout, and not
// when a connection it had kept open turns out to be closed (a client
// library that retries then must have that turned off). Whether to try again
// is the caller's to decide. Nor does it follow a redirect: a 3xx is returned
// as the answer it is.
//
// An answer of any status is a value, 301, 401 and 500 included. An error
// means no answer:
//   - `ErrorKind::Timeout`: nothing came back within `timeout`;
//   - `ErrorKind::Io`: the server was not reached or the connection was lost
//     (no route, a name that does not resolve, TLS, a reset);
//   - `ErrorKind::Config`: the request was never formed (a URL that is not
//     one, a scheme the client does not speak). Nothing was sent.
// With a `Timeout` or an `Io`, the error's `code` is kHttpNotSent when the
// client knows that nothing of the request left this machine: the connection
// was refused, the name did not resolve, the handshake failed, or the time
// ran out before there was a connection. When it is not sure, the code is
// not set. The difference matters for a write: a request whose answer never
// came is in doubt, not failed. An implementation puts no header value and
// nothing of a body into an error's `what`.
class IHttpClient {
 public:
  virtual ~IHttpClient() = default;
  virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};

}  // namespace pychron::http
