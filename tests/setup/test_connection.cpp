// The instrument Test connection and doctor --probe: the shipped NGX profile,
// rendered with a hardware address, connects through the real NGX driver to
// a loopback server speaking the simulated controller's protocol; a closed
// port and a refused login are reported. And the extraction-line page's
// files: the starter line, a lab's own files, and files that do not load or
// do not match.

#include <gtest/gtest.h>

#include <asio.hpp>

#include <random>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/setup/connection.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"

using namespace pychron;
using namespace pychron::setup;
namespace fs = std::filesystem;

namespace {

// Accepts one connection at a time on 127.0.0.1 and plays the simulated NGX
// controller: the banner, then a reply to each "\r"-terminated command.
class NgxServer {
 public:
  explicit NgxServer(std::shared_ptr<spectrometer::NgxSimModel> model)
      : model_(std::move(model)),
        hook_(spectrometer::ngx_sim_hook(model_)),
        acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] {
      while (!stop_) {
        asio::error_code ec;
        asio::ip::tcp::socket socket(io_);
        acceptor_.accept(socket, ec);
        if (ec || stop_) return;
        serve(socket);
      }
    });
  }
  ~NgxServer() {
    stop_ = true;
    // A blocking accept() does not return when another thread closes the
    // acceptor: wake it with a connection of our own.
    asio::error_code ec;
    asio::io_context io;
    asio::ip::tcp::socket wake(io);
    wake.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port_), ec);
    if (thread_.joinable()) thread_.join();
    acceptor_.close(ec);
  }
  std::uint16_t port() const { return port_; }

 private:
  void serve(asio::ip::tcp::socket& s) {
    asio::error_code ec;
    {
      std::lock_guard lock(model_->mutex);
      model_->banner_pending = false;
    }
    asio::write(s, asio::buffer(model_->banner + "\r\n"), ec);
    std::string line;
    char c = 0;
    while (!stop_ && asio::read(s, asio::buffer(&c, 1), ec) == 1 && !ec) {
      line += c;
      if (c != '\r') continue;
      const Bytes reply = hook_(Bytes(line.begin(), line.end()));
      line.clear();
      if (!reply.empty()) asio::write(s, asio::buffer(reply), ec);
      if (ec) return;
    }
  }

  std::shared_ptr<spectrometer::NgxSimModel> model_;
  SimTransport::Hook hook_;
  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

struct Tmp {
  fs::path dir = fs::temp_directory_path() /
                 ("pychron-conn-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                                     std::to_string(std::random_device{}()));
  Tmp() { fs::create_directories(dir); }
  ~Tmp() { fs::remove_all(dir); }
};

ProfileLibrary shipped() {
  const auto r = find_resources();
  auto lib = ProfileLibrary::load(r.profiles, r.examples);
  EXPECT_TRUE(lib) << (lib ? "" : lib.error().what);
  return std::move(*lib);
}

Result<Answers> answers_for(const ResolvedProfile& p, Answers given, const fs::path& root) {
  return complete_answers(p, given, builtin_answers("test", root));
}

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(Connection, TheNgxProfileConnectsThroughTheRealDriverAndLogsIn) {
  auto model = std::make_shared<spectrometer::NgxSimModel>();
  model->values.assign(10, 1.0);
  NgxServer server(model);
  auto lib = shipped();
  auto p = lib.resolve("ngx");
  ASSERT_TRUE(p) << p.error().what;
  Tmp tmp;
  auto a = answers_for(*p,
                       {{"simulation", Value{false}},
                        {"ngx_host", Value{std::string("127.0.0.1")}},
                        {"ngx_port", Value{static_cast<std::int64_t>(server.port())}},
                        {"ngx_user", Value{std::string("lab")}},
                        {"ngx_password", Value{std::string("s3cret")}}},
                       tmp.dir);
  ASSERT_TRUE(a) << a.error().what;
  auto connected = test_instrument_connection(lib, *p, *a);
  ASSERT_TRUE(connected) << connected.error().what;
  EXPECT_NE(connected->find("connected"), std::string::npos) << *connected;
  EXPECT_NE(connected->find("isotopx_ngx at 127.0.0.1:" + std::to_string(server.port())), std::string::npos)
      << *connected;
  std::lock_guard lock(model->mutex);
  EXPECT_EQ(model->user, "lab");
  EXPECT_EQ(model->password, "s3cret");  // from spectrometer.local.toml
}

TEST(Connection, ARefusedLoginAndAClosedPortAreReported) {
  auto lib = shipped();
  auto p = lib.resolve("ngx");
  ASSERT_TRUE(p);
  Tmp tmp;
  {
    auto model = std::make_shared<spectrometer::NgxSimModel>();
    model->login_reply = "E21";
    NgxServer server(model);
    auto a = answers_for(*p,
                         {{"simulation", Value{false}},
                          {"ngx_host", Value{std::string("127.0.0.1")}},
                          {"ngx_port", Value{static_cast<std::int64_t>(server.port())}},
                          {"ngx_user", Value{std::string("lab")}},
                          {"ngx_password", Value{std::string("wrong")}}},
                         tmp.dir);
    ASSERT_TRUE(a) << a.error().what;
    auto refused = test_instrument_connection(lib, *p, *a);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().what.find("E21"), std::string::npos) << refused.error().what;
  }
  // A port nobody listens on: the server above is gone.
  std::uint16_t closed = 0;
  {
    asio::io_context io;
    asio::ip::tcp::acceptor a(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    closed = a.local_endpoint().port();
  }
  auto a = answers_for(*p,
                       {{"simulation", Value{false}},
                        {"ngx_host", Value{std::string("127.0.0.1")}},
                        {"ngx_port", Value{static_cast<std::int64_t>(closed)}}},
                       tmp.dir);
  ASSERT_TRUE(a);
  const auto t0 = std::chrono::steady_clock::now();
  auto unreachable = test_instrument_connection(lib, *p, *a);
  EXPECT_FALSE(unreachable);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(20));
}

TEST(Connection, ASimulatedInstrumentHasNothingToConnectTo) {
  auto lib = shipped();
  auto p = lib.resolve("argus");
  ASSERT_TRUE(p);
  Tmp tmp;
  auto a = answers_for(*p, {}, tmp.dir);  // simulation by default
  ASSERT_TRUE(a);
  auto r = test_instrument_connection(lib, *p, *a);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_NE(r->find("simulated"), std::string::npos) << *r;
}

TEST(ExtractionLine, TheStarterLineOrTheLabsOwnFilesAreCheckedBeforeAnythingIsWritten) {
  auto lib = shipped();
  auto p = lib.resolve("argus");
  ASSERT_TRUE(p);
  Tmp tmp;
  const fs::path examples = find_resources().examples;

  // The starter line is the default.
  auto starter = answers_for(*p, {}, tmp.dir);
  ASSERT_TRUE(starter);
  auto plan = plan_install(lib, *p, *starter, tmp.dir / "starter");
  ASSERT_TRUE(plan) << plan.error().what;

  // The lab's own files are copied in as they are.
  fs::create_directories(tmp.dir / "mine");
  std::string line = read(examples / "extraction_line.toml");
  line += "\n# the lab's line\n";
  std::ofstream(tmp.dir / "mine" / "line.toml", std::ios::binary) << line;  // as written: no CRLF on Windows
  fs::copy_file(examples / "canvas.toml", tmp.dir / "mine" / "drawing.toml");
  auto own = answers_for(*p,
                         {{"line_source", Value{std::string("import")}},
                          {"line_file", Value{(tmp.dir / "mine" / "line.toml").string()}},
                          {"canvas_file", Value{(tmp.dir / "mine" / "drawing.toml").string()}}},
                         tmp.dir);
  ASSERT_TRUE(own) << own.error().what;
  plan = plan_install(lib, *p, *own, tmp.dir / "own");
  ASSERT_TRUE(plan) << plan.error().what;
  ASSERT_TRUE(apply_install(*plan));
  EXPECT_EQ(read(tmp.dir / "own" / "extraction_line.toml"), line);
  EXPECT_EQ(read(tmp.dir / "own" / "canvas.toml"), read(examples / "canvas.toml"));

  // A line without valve A, which the canvas draws.
  std::string renamed = line;
  for (const std::string from : {"name = \"A\"", "interlocks = [\"A\"]"}) {
    const auto at = renamed.find(from);
    ASSERT_NE(at, std::string::npos) << from;
    renamed.replace(at, from.size(), from == "name = \"A\"" ? "name = \"Z\"" : "interlocks = [\"Z\"]");
  }
  std::ofstream(tmp.dir / "mine" / "line.toml", std::ios::binary | std::ios::trunc) << renamed;
  plan = plan_install(lib, *p, *own, tmp.dir / "bad-canvas");
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().what.find("canvas.toml does not match the extraction line"), std::string::npos)
      << plan.error().what;
  EXPECT_FALSE(fs::exists(tmp.dir / "bad-canvas"));

  // A line that does not load, and a file that is not there.
  std::ofstream(tmp.dir / "mine" / "line.toml", std::ios::binary | std::ios::trunc) << "[[valves]]\nname = 3\n";
  plan = plan_install(lib, *p, *own, tmp.dir / "bad-line");
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().what.find("extraction_line.toml is not a usable extraction-line config"), std::string::npos)
      << plan.error().what;
  auto missing = answers_for(*p,
                             {{"line_source", Value{std::string("import")}},
                              {"line_file", Value{(tmp.dir / "nowhere.toml").string()}},
                              {"canvas_file", Value{(tmp.dir / "mine" / "drawing.toml").string()}}},
                             tmp.dir);
  ASSERT_TRUE(missing);
  plan = plan_install(lib, *p, *missing, tmp.dir / "missing");
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().what.find("extraction_line.toml: cannot read"), std::string::npos) << plan.error().what;
}
