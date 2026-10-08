// The seam between this library and the network.
//
// EarthBank is a REST service: a login, lists of controlled terms, and one
// POST per record. This library says what to ask and what to make of the
// answer; it does not open a socket. Everything it sends goes through
// IHttpClient, which a program gives it: the one over HTTPS lives in
// `libs/earthbank_qt`, and the tests have a whole service in memory
// (`tests/earthbank/fake_earthbank.hpp`), because the real one is in
// production, has no sandbox and cannot take a record back.
//
// Beside the interface are the pure helpers a caller needs to write a URL and
// to say in a log what it sent. Nothing here does I/O.
#pragma once

#include <chrono>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::earthbank {

// One request: the method in capitals (`GET`, `POST`, `DELETE`), the whole
// URL with its query already encoded (with_query), the headers as they go on
// the wire, and the body as bytes. `timeout` is for the whole exchange.
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

// Blocking: `send` returns when the answer is in, or when it is known that
// none will be. It is called from one thread at a time.
//
// An answer of any status is a value. An error means no answer:
//   - `ErrorKind::Timeout`: the request went out, or may have, and nothing
//     came back within `timeout`;
//   - `ErrorKind::Io`: the server was not reached or the connection was lost
//     (no route, a name that does not resolve, TLS, a reset). When the client
//     knows that nothing of the request left this machine, the error's `code`
//     is kHttpNotSent;
//   - anything else as its kind says (`Config` for a URL that is not one).
// The difference matters for a write: the service has no way to ask whether a
// record exists, so a POST whose answer never came is in doubt, not failed
// (request_may_have_arrived). An implementation puts no header value and
// nothing of a body into an error's `what`.
class IHttpClient {
 public:
  virtual ~IHttpClient() = default;
  virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};

// The `code` of an error from `send` when the request never left: the
// connection was refused, the name did not resolve, the handshake failed.
// What such a request asked for was not done.
inline constexpr std::string_view kHttpNotSent = "http_not_sent";

// Whether the server may have acted on a request that `send` failed with
// `error`: true unless the client said it was never sent. A timeout is true.
bool request_may_have_arrived(const Error& error);

// `text` as one component of a URL (RFC 3986): letters, digits and `-._~`
// stay, every other byte becomes `%XX` in capitals. A space is `%20`, never
// `+`. `text` is taken as bytes: UTF-8 in gives the UTF-8's escapes out.
std::string url_encode(std::string_view text);

// `url` with `params` as its query, each key and value encoded, in the order
// given: `with_query(base + "/api/core/materials", {{"name.equals", name}})`.
// A `url` that has a query already gets these after it. No params: `url`.
std::string with_query(std::string_view url,
                       std::initializer_list<std::pair<std::string_view, std::string>> params);

// The path of a URL, without scheme, host, query or fragment:
// `/api/core/materials`. A URL with no path has the path `/`.
std::string url_path(std::string_view url);

// The value of a header, its name compared without case; the first if the
// request has it twice.
std::optional<std::string_view> find_header(const HttpRequest& request, std::string_view name);

// A request as it may be written to a log or put into a message: the method
// and the path, `POST /api/arar/ArArAliquot`. Never the query (it holds
// sample names), a header (one holds the token) or the body (one holds the
// password): what a log line says of a request is this and the status.
std::string request_line(const HttpRequest& request);

}  // namespace pychron::earthbank
