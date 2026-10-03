#include "pychron/transport/link_transport.hpp"

namespace pychron {

LinkTransport::LinkTransport(std::string name, std::string link) : name_(std::move(name)), link_(std::move(link)) {}

Unexpected<Error> LinkTransport::misuse() const {
  return fail(ErrorKind::Config,
              "transport '" + name_ + "' is a link to '" + link_ + "'; only drivers that share that link can use it",
              name_);
}

Result<Bytes> LinkTransport::exchange(Bytes, ReadSpec, Duration) { return misuse(); }
Result<void> LinkTransport::write(Bytes) { return misuse(); }
Result<Bytes> LinkTransport::read(ReadSpec, Duration) { return misuse(); }
Result<std::optional<Bytes>> LinkTransport::poll(ReadSpec, Duration) { return misuse(); }
Result<void> LinkTransport::transaction(std::function<Result<void>()>) { return misuse(); }

Health LinkTransport::health() const {
  Health h;
  h.state = HealthState::Connected;
  return h;
}

}  // namespace pychron
