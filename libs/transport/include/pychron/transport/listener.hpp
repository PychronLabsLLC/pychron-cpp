#pragma once

// The server side of a socket, for a program that answers as a device would
// (sim serve, docs/superpowers/specs/2026-10-09-sim-serve-design.md section
// 4.3). Asio stays private to libs/transport, as for the transports.
//
// Every call blocks the calling thread for at most its timeout, in real time:
// a server on sockets is on real time by nature. Each object is used from one
// thread, except close(), which any thread may call: a call waiting in the
// owner's thread then returns Cancelled within 50 ms, and the socket is closed
// when the owner destroys the object (or, for a listener, by close() itself
// once no call is waiting).

#include <cstdint>
#include <memory>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/bytes.hpp"

namespace pychron {

// One accepted TCP connection.
class ByteStream {
 public:
  virtual ~ByteStream() = default;
  // What has arrived, waiting up to `timeout` for anything; empty when nothing
  // came. Io once the peer has closed, Cancelled once close()d.
  virtual Result<Bytes> read_some(Duration timeout) = 0;
  virtual Result<void> write(const Bytes& bytes, Duration timeout) = 0;
  virtual void close() = 0;
};

class TcpListener {
 public:
  // `address` is a numeric IPv4 or IPv6 address; port 0 lets the OS choose.
  // Io, naming address and port, when it cannot be bound.
  static Result<std::unique_ptr<TcpListener>> bind(const std::string& address, std::uint16_t port);
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // The bound port (the OS's choice for port 0).
  std::uint16_t port() const noexcept;
  // The next connection; Timeout after `timeout` with none, Cancelled once close()d.
  Result<std::unique_ptr<ByteStream>> accept(Duration timeout);
  void close();

 private:
  struct Io;
  explicit TcpListener(std::unique_ptr<Io> io);
  std::unique_ptr<Io> io_;
};

class UdpListener {
 public:
  struct Peer;  // a sender's address
  struct Datagram {
    Bytes data;
    std::shared_ptr<const Peer> from;
  };

  static Result<std::unique_ptr<UdpListener>> bind(const std::string& address, std::uint16_t port);
  ~UdpListener();
  UdpListener(const UdpListener&) = delete;
  UdpListener& operator=(const UdpListener&) = delete;

  std::uint16_t port() const noexcept;
  // The next datagram; Timeout after `timeout` with none, Cancelled once close()d.
  Result<Datagram> receive(Duration timeout);
  Result<void> send_to(const Peer& to, const Bytes& bytes);
  void close();

 private:
  struct Io;
  explicit UdpListener(std::unique_ptr<Io> io);
  std::unique_ptr<Io> io_;
};

}  // namespace pychron
