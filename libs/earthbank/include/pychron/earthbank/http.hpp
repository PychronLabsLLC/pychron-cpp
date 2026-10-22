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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/http.hpp"

namespace pychron::earthbank {

using http::HttpRequest;
using http::HttpResponse;
using http::IHttpClient;
using http::kHttpNotSent;

// The interface and its contract are in `pychron/core/http.hpp`. One thing
// is this service's: it has no way to ask whether a record exists, so a POST
// sent twice may be a record made twice, and an implementation never sends a
// request a second time on its own.

// The client a program talks to the service through: the one over HTTPS of
// `libs/earthbank_qt` when this build has it, else null (a program says so and
// does not go on). Defined by that library, or, where it is not built, by the
// program itself (elctl's earthbank_no_network.cpp).
std::unique_ptr<IHttpClient> make_http_client();

// Whether the server may have acted on a request that `send` failed with
// `error`: false when the client said it was never sent (kHttpNotSent) or
// never formed (`Config`), true for everything else. A plain timeout is
// true, and so is a connection that was lost.
bool request_may_have_arrived(const Error& error);

// `text` as one component of a URL (RFC 3986): letters, digits and `-._~`
// stay, every other byte becomes `%XX` in capitals. A space is `%20`, never
// `+`. `text` is taken as bytes: UTF-8 in gives the UTF-8's escapes out.
std::string url_encode(std::string_view text);

// `url` with `params` as its query, each key and value encoded, in the order
// given: `with_query(base + "/api/core/materials", {{"name.equals", name}})`.
// A `url` that has a query already gets these after it, and one with a
// fragment gets them before it. No params: `url`.
std::string with_query(std::string_view url,
                       std::initializer_list<std::pair<std::string_view, std::string>> params);

// The path of a URL, without scheme, host, query or fragment:
// `/api/core/materials`. A URL with no path has the path `/`. A `://` after
// the first `/`, `?` or `#` is not a scheme's: `/x?u=http://h/y` is `/x`.
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
