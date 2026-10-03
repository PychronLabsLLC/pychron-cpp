#pragma once

// A transport with no wire of its own: it names a connection another config
// file owns ([transports.x] kind = "link", link = "<name>"). Drivers that
// share a vendor link (NGX) recognise it and find the owner's link by name;
// to anything else it is a configuration mistake, so every I/O call fails
// Config. open() and close() succeed and do nothing, so loading a config
// that declares one never opens a second socket.

#include <string>

#include "pychron/transport/transport.hpp"

namespace pychron {

class LinkTransport final : public Transport {
 public:
  LinkTransport(std::string name, std::string link);

  const std::string& name() const override { return name_; }
  const std::string& link() const noexcept { return link_; }

  Result<void> open() override { return {}; }
  void close() override {}
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout = kDefaultTimeout) override;
  Result<void> write(Bytes tx) override;
  Result<Bytes> read(ReadSpec rs, Duration timeout = kDefaultTimeout) override;
  Result<std::optional<Bytes>> poll(ReadSpec rs, Duration timeout = kDefaultTimeout) override;
  Result<void> transaction(std::function<Result<void>()> body) override;
  // Connected: a link has no health of its own; the owner's transport does.
  Health health() const override;

 private:
  Unexpected<Error> misuse() const;
  std::string name_, link_;
};

}  // namespace pychron
