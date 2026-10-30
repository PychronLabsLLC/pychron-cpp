#include "pychron/devices/spectrometer/qtegra_link.hpp"

#include <mutex>

#include "pychron/transport/udp_transport.hpp"

namespace pychron::spectrometer {

namespace q = codec::qtegra;

namespace {

// A server that drops KeepAlive is a script older than it or one listening on
// UDP: say so, keeping the kind.
Error with_keep_alive_hint(Error error) {
  error.what = "KeepAlive: " + error.what +
               "; RemoteControlServer.cs must be current and run with Config.use_udp = false";
  return error;
}

}  // namespace

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
  return open_session(transport);
}

Result<void> QtegraLink::open_session(Transport& transport) {
  session_open_ = false;
  const auto opened = [&]() -> Result<void> {
    if (!keep_alive_) {
      auto cmd = q::keep_alive(terminator_);
      if (!cmd) return fail(std::move(cmd).error());
      auto reply = transport.exchange(cmd->tx, *cmd->reply);
      if (!reply) return fail(with_keep_alive_hint(std::move(reply).error()));
      if (auto ok = q::decode_ok(*reply); !ok) return fail(with_keep_alive_hint(std::move(ok).error()));
      keep_alive_ = true;
    }
    auto cmd = q::describe(terminator_);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = transport.exchange(cmd->tx, *cmd->reply);
    if (!reply) return fail(std::move(reply).error());
    auto server = q::decode_describe(*reply);
    if (!server) return fail(std::move(server).error());
    if (auto ok = q::check_server(*server); !ok) return ok;
    return handshake_ ? handshake_(transport, *server) : Result<void>{};
  }();
  if (opened) {
    session_open_ = true;
  } else if (is_transient(opened.error().kind)) {
    keep_alive_ = false;
  }
  return opened;
}

Result<void> QtegraLink::handshake() {
  // In a transaction, as the Link's reconnect runs it: the mutex is then
  // always taken after the transport, so the two cannot wait on each other.
  return transport_.transaction([this] { return run_handshake(transport_); });
}

Result<Bytes> QtegraLink::exchange(const codec::Command& command, Resend resend) {
  if (!command.reply) return fail(ErrorKind::Config, "Qtegra command expects no reply");
  {
    std::lock_guard lock(handshake_mutex_);
    if (!session_open_) {
      if (auto opened = open_session(transport_); !opened) return fail(std::move(opened).error());
    }
  }
  auto reply = transport_.exchange(command.tx, *command.reply, Transport::kDefaultTimeout, resend);
  if (!reply && is_transient(reply.error().kind)) {
    std::lock_guard lock(handshake_mutex_);
    keep_alive_ = false;
    session_open_ = false;
  }
  return reply;
}

Result<Bytes> QtegraLink::ask(std::string_view text) {
  codec::Command command;
  command.tx = to_bytes(std::string(text) + std::string(codec::qtegra::terminator_text(terminator_)));
  command.reply = codec::qtegra::reply_spec();
  const std::function<Result<Bytes>()> op = [&] { return exchange(command, Resend::Allowed); };
  return link_.call(op, Repeat::Safe);
}

Result<QtegraLinkHandle> make_qtegra_link(Transport& transport, const std::string& name,
                                          codec::qtegra::Terminator terminator, const Clock& clock,
                                          LinkPolicy policy) {
  if (dynamic_cast<const UdpTransport*>(&transport) != nullptr) {
    return fail(ErrorKind::Config,
                "Qtegra over UDP is not supported: give it a tcp transport and run RemoteControlServer.cs with "
                "Config.use_udp = false");
  }
  return QtegraLinkHandle::make(transport, name, [&](Transport& t) {
    return std::make_shared<QtegraLink>(t, terminator, clock, policy);
  });
}

}  // namespace pychron::spectrometer
