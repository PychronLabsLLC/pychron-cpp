#include "pychron/devices/spectrometer/qtegra_link.hpp"

#include <mutex>

namespace pychron::spectrometer {

QtegraLink::QtegraLink(Transport& transport, codec::qtegra::Terminator terminator, const Clock& clock)
    : transport_(transport), terminator_(terminator), reconnector_(transport, clock), handshake_mutex_(clock) {}

void QtegraLink::set_handshake(Handshake handshake) {
  std::lock_guard lock(handshake_mutex_);
  handshake_ = std::move(handshake);
}

Result<void> QtegraLink::handshake() {
  std::lock_guard lock(handshake_mutex_);
  if (!handshake_) return {};
  return handshake_(transport_);
}

Result<Bytes> QtegraLink::exchange(const codec::Command& command) {
  if (!command.reply) return fail(ErrorKind::Config, "Qtegra command expects no reply");
  return reconnector_.run<Bytes>([&] { return transport_.exchange(command.tx, *command.reply); },
                                 [this] { return handshake(); });
}

Result<Bytes> QtegraLink::ask(std::string_view text) {
  codec::Command command;
  command.tx = to_bytes(std::string(text) + std::string(codec::qtegra::terminator_text(terminator_)));
  command.reply = codec::qtegra::reply_spec();
  return exchange(command);
}

Result<QtegraLinkHandle> make_qtegra_link(Transport& transport, const std::string& name,
                                          codec::qtegra::Terminator terminator, const Clock& clock) {
  return QtegraLinkHandle::make(transport, name, [&](Transport& t) {
    return std::make_shared<QtegraLink>(t, terminator, clock);
  });
}

}  // namespace pychron::spectrometer
