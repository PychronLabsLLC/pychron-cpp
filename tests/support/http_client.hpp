#pragma once

// A blocking HTTP client for the server tests: one request, read to the end
// of the stream.

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace http_client {

inline asio::ip::tcp::socket connect(asio::io_context& io, std::uint16_t port, const char* host = "127.0.0.1") {
  asio::ip::tcp::socket socket(io);
  socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address(host), port));
  return socket;
}

// Everything the server sends until it closes the connection.
inline std::string read_all(asio::ip::tcp::socket& socket) {
  std::string out;
  char buf[4096];
  asio::error_code ec;
  for (;;) {
    const std::size_t n = socket.read_some(asio::buffer(buf), ec);
    out.append(buf, n);
    if (ec) break;
  }
  return out;
}

// read_all, but nullopt when the server has not closed within `limit`.
inline std::optional<std::string> read_all_within(asio::ip::tcp::socket& socket, std::chrono::milliseconds limit) {
  auto done = std::async(std::launch::async, [&socket] { return read_all(socket); });
  if (done.wait_for(limit) == std::future_status::ready) return done.get();
  // shutdown() wakes a reader blocked in the kernel on every platform and
  // leaves the socket object alone, which the reader is still using.
  asio::error_code ec;
  socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
  done.wait();
  return std::nullopt;
}

// Sends `request` (in two halves `piece_delay` apart when that is not zero)
// and returns the whole response.
inline std::string http(std::uint16_t port, std::string_view request,
                        std::chrono::milliseconds piece_delay = std::chrono::milliseconds(0),
                        const char* host = "127.0.0.1") {
  asio::io_context io;
  asio::ip::tcp::socket socket = connect(io, port, host);
  asio::error_code ec;
  if (piece_delay.count() == 0) {
    asio::write(socket, asio::buffer(request.data(), request.size()), ec);
  } else {
    const std::size_t half = request.size() / 2;
    asio::write(socket, asio::buffer(request.data(), half), ec);
    std::this_thread::sleep_for(piece_delay);
    asio::write(socket, asio::buffer(request.data() + half, request.size() - half), ec);
  }
  return read_all(socket);
}

inline std::string get(std::uint16_t port, std::string_view target, const char* host = "127.0.0.1") {
  return http(port, "GET " + std::string(target) + " HTTP/1.1\r\nHost: x\r\n\r\n", std::chrono::milliseconds(0), host);
}

inline std::string body(const std::string& response) {
  const auto at = response.find("\r\n\r\n");
  return at == std::string::npos ? std::string() : response.substr(at + 4);
}

}  // namespace http_client
