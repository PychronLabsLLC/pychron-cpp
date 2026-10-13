// make_http_client() in a build without libs/earthbank_qt (no Qt6 Network):
// there is no client, and `elctl earthbank` says so where it would use one.

#include <memory>

#include "pychron/earthbank/http.hpp"

namespace pychron::earthbank {

std::unique_ptr<IHttpClient> make_http_client() { return nullptr; }

}  // namespace pychron::earthbank
