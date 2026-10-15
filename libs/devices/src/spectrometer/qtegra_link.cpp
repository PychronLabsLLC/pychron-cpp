#include "pychron/devices/spectrometer/qtegra_link.hpp"

#include <mutex>

namespace pychron::spectrometer {

QtegraLink::QtegraLink(Transport& transport, codec::qtegra::Terminator terminator, const Clock& clock,
                       LinkPolicy policy)
    : transport_(transport), terminator_(terminator), handshake_mutex_(clock), link_(transport, clock, policy) {
  link_.set_connect([this](Transport& t) { return run_handshake(t); });
}

void QtegraLink::set_handshake(Handshake handshake) {
  std::lock_guard lock(handshake_mutex_);
  handshake_ = std::move(handshake);
}

Result<void> QtegraLink::run_handshake(Transport& transport) {
  std::lock_guard lock(handshake_mutex_);
  if (!handshake_) return {};
  return handshake_(transport);
}

Result<void> QtegraLink::handshake() {
  // In a transaction, as the Link's reconnect runs it: the mutex is then
  // always taken after the transport, so the two cannot wait on each other.
  return transport_.transaction([this] { return run_handshake(transport_); });
}

Result<Bytes> QtegraLink::exchange(const codec::Command& command, Resend resend) {
  if (!command.reply) return fail(ErrorKind::Config, "Qtegra command expects no reply");
  return transport_.exchange(command.tx, *command.reply, Transport::kDefaultTimeout, resend);
}

Result<Bytes> QtegraLink::ask(std::string_view text) {
  codec::Command command;
  command.tx = to_bytes(std::string(text) + std::string(codec::qtegra::terminator_text(terminator_)));
  command.reply = codec::qtegra::reply_spec();
  const std::function<Result<Bytes>()> op = [&] { return exchange(command, Resend::Allowed); };
  return link_.call(op, Repeat::Safe);
}

Result<QtegraLinkHandle> make_qtegra_link(Transport& transport, const std::string& name,
                                          codec::qtegra::Terminator terminator, const Clock& clock) {
  return QtegraLinkHandle::make(transport, name, [&](Transport& t) {
    return std::make_shared<QtegraLink>(t, terminator, clock);
  });
}

}  // namespace pychron::spectrometer
