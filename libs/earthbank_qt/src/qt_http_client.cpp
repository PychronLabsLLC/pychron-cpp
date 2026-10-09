#include "pychron/earthbank/qt_http_client.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QList>
#include <QNetworkProxy>
#include <QSslError>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <QUrl>

// A small HTTP/1.1 client over QSslSocket, and not QNetworkAccessManager: the
// seam (http.hpp) promises that a request is sent once and a redirect is not
// followed, and the manager does neither of those promises' work for us. It
// re-sends a request whose connection was closed under it (twice, with no
// switch to turn that off), and what it has sent is not something it tells.
// Here a request is one connection, one write and one read, and the code
// knows whether a byte of it was written.

namespace pychron::earthbank {
namespace {

// The most of an answer that is kept. The largest the service gives is a page
// of a list; a body past this is a fault, not a list.
constexpr std::size_t kMostAnswer = 64u * 1024u * 1024u;
// The head of an answer, status line and headers.
constexpr std::size_t kMostHead = 64u * 1024u;

// Qt needs a QCoreApplication for its event loops and its sockets. A program
// that has none gets one here, once, which lives to the end of the process:
// the least a blocking client can ask of a program that never thought of Qt.
// (argc and argv are Qt's to keep a reference to.)
void ensure_application() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (QCoreApplication::instance() != nullptr) return;
    static int argc = 1;
    static char name[] = "pychron";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application(argc, argv);
  });
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool same_name(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

bool is_token(std::string_view text) {
  if (text.empty()) return false;
  return std::all_of(text.begin(), text.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
  });
}

// A header value that cannot be written on one line is a header injected.
bool is_field_value(std::string_view text) {
  return std::none_of(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

// ---------------------------------------------------------------- the answer

// An answer as it is read, in pieces: the status line and headers, then the
// body as the head says it goes (a length, chunks, or until the connection
// closes). Nothing is interpreted beyond that: no encoding is asked for and
// none is undone.
class AnswerReader {
 public:
  enum class State { More, Done, Bad };

  explicit AnswerReader(bool head_request) : head_request_(head_request) {}

  State feed(const QByteArray& data) {
    buffer_.append(data);
    return step();
  }

  // The connection closed: an answer that goes on to the end of it is done,
  // any other is cut short.
  State closed() {
    if (state_ == State::More && phase_ == Phase::UntilClose) state_ = State::Done;
    if (state_ == State::More) fault_ = "the connection was closed before the answer was complete";
    if (state_ == State::More) state_ = State::Bad;
    return state_;
  }

  int status() const { return status_; }
  std::string take_body() { return std::move(body_); }
  const std::string& fault() const { return fault_; }

 private:
  enum class Phase { Head, Length, ChunkSize, ChunkData, ChunkEnd, Trailer, UntilClose };

  State bad(std::string why) {
    fault_ = std::move(why);
    return state_ = State::Bad;
  }
  State done() { return state_ = State::Done; }

  State step() {
    if (state_ != State::More) return state_;
    for (;;) {
      switch (phase_) {
        case Phase::Head: {
          const qsizetype end = buffer_.indexOf("\r\n\r\n");
          if (end < 0) {
            if (static_cast<std::size_t>(buffer_.size()) > kMostHead) return bad("the answer's headers are too long");
            return State::More;
          }
          const std::string head = buffer_.left(end).toStdString();
          buffer_.remove(0, end + 4);
          if (const auto fault = read_head(head)) return bad(*fault);
          if (status_ >= 100 && status_ < 200) {
            if (status_ == 101) return bad("the service switched protocols");
            continue;  // an interim answer; the real one follows
          }
          if (head_request_ || status_ == 204 || status_ == 304) return done();
          if (chunked_) {
            phase_ = Phase::ChunkSize;
          } else if (length_) {
            phase_ = Phase::Length;
          } else {
            phase_ = Phase::UntilClose;
          }
          continue;
        }
        case Phase::Length: {
          if (*length_ > kMostAnswer) return bad("the answer is too large");
          if (static_cast<std::size_t>(buffer_.size()) < *length_) return State::More;
          body_.assign(buffer_.constData(), *length_);
          return done();
        }
        case Phase::UntilClose: {
          if (static_cast<std::size_t>(buffer_.size()) > kMostAnswer) return bad("the answer is too large");
          body_ = buffer_.toStdString();
          return State::More;
        }
        case Phase::ChunkSize: {
          const qsizetype eol = buffer_.indexOf("\r\n");
          if (eol < 0) return static_cast<std::size_t>(buffer_.size()) > 1024 ? bad("a chunk's size is not one") : State::More;
          std::string_view line(buffer_.constData(), static_cast<std::size_t>(eol));
          line = trim(line.substr(0, line.find(';')));
          std::size_t size = 0;
          const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), size, 16);
          if (line.empty() || error != std::errc() || end != line.data() + line.size()) return bad("a chunk's size is not one");
          buffer_.remove(0, eol + 2);
          if (size == 0) {
            phase_ = Phase::Trailer;
          } else {
            if (body_.size() + size > kMostAnswer) return bad("the answer is too large");
            chunk_ = size;
            phase_ = Phase::ChunkData;
          }
          continue;
        }
        case Phase::ChunkData: {
          if (static_cast<std::size_t>(buffer_.size()) < chunk_) return State::More;
          body_.append(buffer_.constData(), chunk_);
          buffer_.remove(0, static_cast<qsizetype>(chunk_));
          phase_ = Phase::ChunkEnd;
          continue;
        }
        case Phase::ChunkEnd: {
          if (buffer_.size() < 2) return State::More;
          if (!buffer_.startsWith("\r\n")) return bad("a chunk does not end where it should");
          buffer_.remove(0, 2);
          phase_ = Phase::ChunkSize;
          continue;
        }
        case Phase::Trailer: {
          // Trailer fields, if any, up to the empty line that ends the answer.
          const qsizetype eol = buffer_.indexOf("\r\n");
          if (eol < 0) return State::More;
          const bool empty = eol == 0;
          buffer_.remove(0, eol + 2);
          if (empty) return done();
          continue;
        }
      }
    }
  }

  // The status line and the headers that say how the body goes.
  std::optional<std::string> read_head(const std::string& head) {
    chunked_ = false;
    length_.reset();
    std::size_t at = head.find("\r\n");
    const std::string_view first(head.data(), at == std::string::npos ? head.size() : at);
    if (first.size() < 12 || !first.starts_with("HTTP/1.") || first[8] != ' ') return "the answer is not HTTP";
    int status = 0;
    const auto [end, error] = std::from_chars(first.data() + 9, first.data() + 12, status);
    if (error != std::errc() || end != first.data() + 12 || status < 100 || status > 599) return "the answer's status is not one";
    status_ = status;
    while (at != std::string::npos) {
      const std::size_t from = at + 2;
      at = head.find("\r\n", from);
      const std::string_view line(head.data() + from, (at == std::string::npos ? head.size() : at) - from);
      const std::size_t colon = line.find(':');
      if (colon == std::string_view::npos) continue;
      const std::string_view name = line.substr(0, colon);
      const std::string_view value = trim(line.substr(colon + 1));
      if (same_name(name, "transfer-encoding")) {
        std::string coding(value);
        std::transform(coding.begin(), coding.end(), coding.begin(), lower);
        chunked_ = coding.size() >= 7 && coding.compare(coding.size() - 7, 7, "chunked") == 0;
        if (!chunked_) return "the answer has a transfer coding this does not read";
      } else if (same_name(name, "content-length")) {
        std::size_t length = 0;
        const auto [stop, bad_length] = std::from_chars(value.data(), value.data() + value.size(), length);
        if (value.empty() || bad_length != std::errc() || stop != value.data() + value.size())
          return "the answer's length is not a number";
        if (length_ && *length_ != length) return "the answer gives two lengths";
        length_ = length;
      }
    }
    if (chunked_) length_.reset();
    return std::nullopt;
  }

  bool head_request_;
  State state_ = State::More;
  Phase phase_ = Phase::Head;
  QByteArray buffer_;
  int status_ = 0;
  bool chunked_ = false;
  std::optional<std::size_t> length_;
  std::size_t chunk_ = 0;
  std::string body_, fault_;
};

// ---------------------------------------------------------------- the request

// The bytes of a request: the line, the headers, the body. The ones that say
// where it goes, how long it is and that the connection ends with it are this
// client's, whatever the request has of them.
std::string wire_form(const HttpRequest& request, const QUrl& url) {
  std::string target = url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority | QUrl::RemoveFragment).toStdString();
  if (target.empty()) target = "/";
  std::string host = url.host(QUrl::FullyEncoded).toStdString();
  if (host.find(':') != std::string::npos) host = "[" + host + "]";
  const int port = url.port();
  if (port > 0) host += ":" + std::to_string(port);

  std::string out = request.method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\n";
  bool has_agent = false;
  for (const auto& [name, value] : request.headers) {
    if (same_name(name, "host") || same_name(name, "content-length") || same_name(name, "connection") ||
        same_name(name, "transfer-encoding") || same_name(name, "expect")) {
      continue;
    }
    has_agent = has_agent || same_name(name, "user-agent");
    out += name + ": " + value + "\r\n";
  }
  if (!has_agent) out += std::string("User-Agent: ") + PYCHRON_EARTHBANK_USER_AGENT + "\r\n";
  const bool carries = request.method == "POST" || request.method == "PUT" || request.method == "PATCH";
  if (carries || !request.body.empty()) out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  out += "Connection: close\r\n\r\n";
  out += request.body;
  return out;
}

// The request as a fault of its form, or nothing: the URL, the method, and
// every header on one line.
std::optional<std::string> misformed(const HttpRequest& request, QUrl& url) {
  url = QUrl::fromEncoded(QByteArray::fromStdString(request.url), QUrl::StrictMode);
  const QString scheme = url.scheme();
  if (!url.isValid() || url.host().isEmpty() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
    return "the request is not addressed to an http or https URL";
  if (!url.userInfo().isEmpty()) return "the request's URL names a user";
  if (!is_token(request.method)) return "the request has no valid method";
  for (const auto& [name, value] : request.headers) {
    if (!is_token(name) || !is_field_value(value)) return "a header of the request is not one";
  }
  return std::nullopt;
}

// The proxy the system names for this address, when it is not this machine:
// Qt's classes for a socket do not look for one themselves.
void use_system_proxy(QSslSocket& socket, const QUrl& url) {
  const QHostAddress address(url.host());
  if (url.host() == QLatin1String("localhost") || (!address.isNull() && address.isLoopback())) {
    socket.setProxy(QNetworkProxy::NoProxy);
    return;
  }
  const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
  for (const QNetworkProxy& proxy : proxies) {
    if (proxy.type() == QNetworkProxy::HttpProxy || proxy.type() == QNetworkProxy::Socks5Proxy) {
      socket.setProxy(proxy);
      return;
    }
  }
  socket.setProxy(QNetworkProxy::NoProxy);
}

Error failure_of(ErrorKind kind, std::string what, bool not_sent) {
  Error error{kind, std::move(what), ""};
  if (not_sent) error.code = std::string(kHttpNotSent);
  return error;
}

}  // namespace

Result<HttpResponse> QtHttpClient::send(const HttpRequest& request) {
  QUrl url;
  if (const auto fault = misformed(request, url)) return fail(ErrorKind::Config, "earthbank: " + *fault + "; nothing was sent");
  ensure_application();

  const bool secure = url.scheme() == QLatin1String("https");
  const std::string wire = wire_form(request, url);
  const bool head_request = request.method == "HEAD";

  // Declared in the order they are undone: the socket outlives the loop, so
  // that no signal of it reaches a lambda that is gone.
  QSslSocket socket;
  use_system_proxy(socket, url);
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  AnswerReader reader(head_request);

  bool written = false;  // a byte of the request was handed to the socket
  bool settled = false;
  std::optional<Error> failed;
  const auto fail_with = [&](ErrorKind kind, std::string what) {
    if (settled) return;
    settled = true;
    // Whatever went wrong before the request was written, nothing of it left.
    failed = failure_of(kind, std::move(what), !written);
    loop.quit();
  };
  const auto succeed = [&] {
    if (settled) return;
    settled = true;
    loop.quit();
  };
  const auto read_more = [&] {
    if (settled || !written) return;
    switch (reader.feed(socket.readAll())) {
      case AnswerReader::State::Done: return succeed();
      case AnswerReader::State::Bad: return fail_with(ErrorKind::Io, "the answer could not be read: " + reader.fault());
      case AnswerReader::State::More: return;
    }
  };
  const auto lost = [&](const std::string& why) {
    if (settled) return;
    if (written) {
      switch (reader.feed(socket.readAll())) {
        case AnswerReader::State::Done: return succeed();
        case AnswerReader::State::Bad: return fail_with(ErrorKind::Io, "the answer could not be read: " + reader.fault());
        case AnswerReader::State::More: break;
      }
      if (reader.closed() == AnswerReader::State::Done) return succeed();
      return fail_with(ErrorKind::Io, "the connection was lost before the answer was complete: " + why);
    }
    fail_with(ErrorKind::Io, "could not reach the service: " + why);
  };
  const auto write_request = [&] {
    if (settled || written) return;
    written = true;
    socket.write(wire.data(), static_cast<qint64>(wire.size()));
  };

  QObject::connect(&socket, &QAbstractSocket::connected, &loop, [&] {
    if (!secure) write_request();
  });
  QObject::connect(&socket, &QSslSocket::encrypted, &loop, write_request);
  QObject::connect(&socket, &QIODevice::readyRead, &loop, read_more);
  QObject::connect(&socket, &QAbstractSocket::disconnected, &loop, [&] { lost("the server closed the connection"); });
  QObject::connect(&socket, &QAbstractSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) {
    std::string why = socket.errorString().toStdString();
    if (secure && !written) {
      for (const QSslError& e : socket.sslHandshakeErrors()) why += "; " + e.errorString().toStdString();
    }
    lost(why);
  });
  QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
    if (settled) return;
    settled = true;
    failed = failure_of(ErrorKind::Timeout, "the service did not answer within " + std::to_string(request.timeout.count()) + " ms",
                        !written);
    loop.quit();
  });

  timer.start(static_cast<int>(std::min<std::chrono::milliseconds::rep>(request.timeout.count(), 0x7fffffff)));
  const QString host = url.host();
  const quint16 port = static_cast<quint16>(url.port(secure ? 443 : 80));
  if (secure) {
    socket.connectToHostEncrypted(host, port);
  } else {
    socket.connectToHost(host, port);
  }
  if (!settled) loop.exec();
  socket.abort();

  if (failed) return fail(std::move(*failed));
  return HttpResponse{reader.status(), reader.take_body()};
}

std::unique_ptr<IHttpClient> make_http_client() { return std::make_unique<QtHttpClient>(); }

}  // namespace pychron::earthbank
