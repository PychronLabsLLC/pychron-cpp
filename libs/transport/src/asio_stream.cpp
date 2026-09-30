#include "asio_stream.hpp"

namespace pychron::detail {

Error io_error(std::string_view what, const asio::error_code& ec) {
  return Error{ErrorKind::Io, std::string(what) + ": " + ec.message(), {}};
}

std::string millis(Duration d) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()) + " ms";
}

}  // namespace pychron::detail
