// EarthBank, in memory, for tests.
//
// The real service is in production, has no sandbox and offers no way to take
// a record back, so no test may talk to it. This is what the tests of the
// REST route talk to instead: an IHttpClient that answers as the service
// does, keeps what it is sent, and can be told to fail.
//
// What it does is what the legacy integration found the service to do (the
// University of Melbourne fork of pychron: its client, its README and its
// probe scripts), traps included: a package created from a flat body loses
// its name, a data point without its wrapper is "not writable", a core lookup
// wants a login and an Ar-Ar one does not. Each such behaviour carries a
// comment in fake_earthbank.cpp saying where it was learned. Where legacy
// does not say what the service does, the plainest thing was chosen and
// marked `// assumed (n):` there; all of those are listed in
// fixtures/README.md, by that number, for someone with a login to check.
// Where the plainest thing is only one of two a client must live with, a
// control gives the other (the last group of them below).
//
// It starts from the files in fixtures/ (the lookups, one account, a few
// packages), all hand-written with invented people and ids. The ids it gives
// out are counted up from fixed numbers, a range per kind of record: the same
// calls give the same ids, and an id of one kind is never one of another.
//
// Linked into tests only (target `pychron::earthbank_fake`). One lock guards
// it, so a worker thread and the test's may both use it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"

namespace pychron::earthbank::testing {

// One request the fake was sent, as it is kept: with every secret already
// taken out. The value of an `Authorization` header (and of any other that
// carries a credential) and of a `password` or a token in a JSON body is
// `<redacted>`; a body that is not JSON on the login path is redacted whole.
// So nothing a failing assertion prints of the log can leak one.
struct LoggedRequest {
  enum class Outcome {
    Answered,  // the client got `status`
    Dropped,   // carried out, and the answer lost: the client got an error (drop_next)
    Lost,      // not carried out, and no answer either: the same error (lose_next)
    NotSent,   // never arrived: the client got an error that says so (refuse_next)
  };
  std::string method;
  std::string path;
  // Decoded, in the order sent; the value of a key that looks like a
  // credential's (`access_token`, `password`, ...) is `<redacted>`.
  std::vector<std::pair<std::string, std::string>> query;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  // What the service answered; for Dropped what it would have; 0 for Lost
  // and NotSent.
  int status = 0;
  Outcome outcome = Outcome::Answered;
  // It carried a bearer token the service took.
  bool authenticated = false;

  // The value of a query parameter, if it was sent.
  std::optional<std::string> param(std::string_view key) const;
};

// `POST /api/arar/ArArAliquot -> 201`, then the headers and the start of the
// body. This is what gtest prints of a request.
std::ostream& operator<<(std::ostream& out, const LoggedRequest& request);

// What the service holds. A package is the row the service lists it as
// (`{"dataPackageDTO": {...}, "count...": n}`). Every other record is kept as
// it was sent, with the ids the service gave it filled in, under the package
// it is in: for a data point and its sub-records, the package of the
// data point's umbrella.
struct Store {
  struct Record {
    std::int64_t id = 0;
    std::int64_t package_id = 0;
    nlohmann::json body;
  };
  std::vector<nlohmann::json> packages;
  std::vector<Record> samples;      // body: the wrapper, {sampleDTO, locationDTO, ...}
  std::vector<Record> data_points;  // id: the arArDataPointId; body: the wrapper
  std::vector<Record> aliquots, measurements, age_calcs, age_summaries;
};

// A script that has not fired yet (FakeEarthBank::pending): `kind` is `fail`,
// `drop`, `lose` or `refuse`, `skip` the requests it will still let by.
struct PendingScript {
  std::string kind, method, path_prefix;
  std::size_t skip = 0;
  bool operator==(const PendingScript&) const = default;
};
std::ostream& operator<<(std::ostream& out, const PendingScript& script);

// Where the answer to a new sample carries the sample's id.
enum class SampleIdPlace {
  SampleDto,  // `sampleDTO.id`: what legacy reads first
  Sample,     // `sample.id`, the sample under that key: what legacy reads second
  Top,        // `id` on the wrapper, and none in `sampleDTO`: as other records answer
};

class FakeEarthBank final : public IHttpClient {
 public:
  struct Options {
    // Scheme and host: a request to anything else never arrives.
    std::string base_url = "https://earthbank.test";
    // The password of the account in fixtures/account.json (its `login` is the
    // user name). It opens nothing.
    std::string password = "not-a-real-password";
    std::filesystem::path fixtures = PYCHRON_EARTHBANK_FIXTURE_DIR;
  };

  // Throws std::runtime_error naming the file if a fixture is missing or is
  // not what it should be.
  FakeEarthBank();
  explicit FakeEarthBank(Options options);

  Result<HttpResponse> send(const HttpRequest& request) override;

  // ---- What a test needs to know to talk to it ------------------------------
  const std::string& base_url() const { return options_.base_url; }
  const std::string& login() const { return login_; }
  std::string password() const;
  // A token as a login would give, without the request.
  std::string issue_token();
  // The id of `name` (exactly) in the vocabulary at `endpoint`.
  std::optional<std::int64_t> lookup_id(std::string_view endpoint, std::string_view name) const;
  // The first fixture package the account can write, and the first it cannot.
  std::int64_t writable_package() const;
  std::int64_t read_only_package() const;
  bool can_write(std::int64_t package_id) const;

  // ---- Arranging the service ------------------------------------------------
  // A package made by somebody else, with the next package id.
  std::int64_t add_package(std::string_view name, bool writable, std::int64_t institution_id = 0);
  void set_writable(std::int64_t package_id, bool writable);
  // One more row of a vocabulary: an object with at least `id` and `name`.
  void add_lookup_row(std::string_view endpoint, nlohmann::json row);
  // The account's password is changed elsewhere: every token is void and the
  // old password is refused.
  void set_password(std::string password);

  // ---- Making it fail -------------------------------------------------------
  // Every token given so far is void: a request with one is 401 until the
  // client logs in again.
  void expire_tokens();
  // The four scripts. Each is for one request of `method` whose path begins
  // with `path_prefix`: the first such, or with `skip` the one after that
  // many (`skip` 2: the third). Several may wait. Every waiting script counts
  // every request that fits it; when more than one is due, the oldest fires
  // and the others fire on the requests after.
  //
  // The request is answered with this and nothing is done.
  void fail_next(std::string_view method, std::string_view path_prefix, int status, std::string body,
                 std::size_t skip = 0);
  // The request is carried out and its answer lost: the record is there and
  // the client gets `error`, an `ErrorKind::Timeout` or a plain
  // `ErrorKind::Io` (no kHttpNotSent). Another kind: std::invalid_argument.
  void drop_next(std::string_view method, std::string_view path_prefix, ErrorKind error = ErrorKind::Timeout,
                 std::size_t skip = 0);
  // The request is not carried out and gets no answer either: nothing is
  // done, and the client gets the same `error` drop_next would give. It
  // cannot tell the two apart.
  void lose_next(std::string_view method, std::string_view path_prefix, ErrorKind error = ErrorKind::Timeout,
                 std::size_t skip = 0);
  // The request never arrives: nothing is done, and the client gets an
  // `ErrorKind::Io` with the code kHttpNotSent.
  void refuse_next(std::string_view method, std::string_view path_prefix, std::size_t skip = 0);
  // The scripts that have not fired, oldest first. Empty at the end of a
  // test whose scripts were all used.
  std::vector<PendingScript> pending() const;

  // ---- What is not known of the service, either way -------------------------
  // Each default is an assumption (fixtures/README.md); a client has to be
  // right under both.
  //
  // The sample search stops minding `dataPackageId.equals` and gives the
  // samples of every package (default: it minds it). Legacy checks each row's
  // package itself for this case.
  void ignore_sample_package_filter(bool ignore);
  // The status of a constraint failure (an id that is no row of its list):
  // 400 (default) or 500. The body is the same.
  void constraint_status(int status);
  // The status of everything created: 201 (default) or 200.
  void created_status(int status);
  // Where the answer to a new sample has its id (default: SampleDto). What
  // is kept, and what a search gives, does not change.
  void sample_id_place(SampleIdPlace place);

  // ---- Looking at what happened ---------------------------------------------
  // Every request, in order, secrets removed.
  std::vector<LoggedRequest> requests() const;
  // How many requests of `method` had a path beginning with `path_prefix`.
  std::size_t count(std::string_view method, std::string_view path_prefix) const;
  Store store() const;

 private:
  struct Scripted {
    enum class Kind { Fail, Drop, Lose, Refuse };
    Kind kind;
    std::string method, path_prefix;
    std::size_t skip = 0;
    int status = 0;
    std::string body;
    ErrorKind error = ErrorKind::Timeout;  // Drop and Lose: what the client gets
  };
  void script(Scripted scripted);
  struct Call;  // one parsed request (fake_earthbank.cpp)

  HttpResponse handle(const Call& call);
  HttpResponse authenticate(const Call& call);
  HttpResponse lookup(const Call& call, const std::vector<nlohmann::json>& rows) const;
  HttpResponse list_packages(const Call& call) const;
  HttpResponse create_package(const Call& call);
  HttpResponse one_package(const Call& call, std::int64_t id);
  HttpResponse find_samples(const Call& call) const;
  HttpResponse create_sample(const Call& call);
  HttpResponse create_data_point(const Call& call);
  HttpResponse create_sub_record(const Call& call, std::string_view table, std::vector<Store::Record>& into,
                                 std::int64_t& next_id);

  std::optional<HttpResponse> vocabulary_fault(std::string_view path, const nlohmann::json& dto,
                                               std::string_view table) const;
  std::string new_token();
  nlohmann::json package_row(const nlohmann::json& row) const;
  const nlohmann::json* find_package(std::int64_t id) const;
  bool can_write_locked(std::int64_t package_id) const;
  std::optional<Scripted> take_scripted(std::string_view method, std::string_view path);

  Options options_;
  mutable std::mutex mutex_;

  nlohmann::json account_;
  std::string login_;
  std::int64_t person_id_ = 0;  // `createdById` of what the account creates
  std::set<std::string> tokens_;
  int tokens_issued_ = 0;

  std::map<std::string, std::vector<nlohmann::json>, std::less<>> lookups_;  // by endpoint
  std::set<std::int64_t> writable_;
  Store store_;
  std::vector<Scripted> scripted_;
  bool ignore_sample_package_filter_ = false;
  int constraint_status_ = 400;
  int created_status_ = 201;
  SampleIdPlace sample_id_place_ = SampleIdPlace::SampleDto;
  std::vector<LoggedRequest> log_;

  std::int64_t next_package_ = 620001;
  std::int64_t next_sample_ = 710001;
  std::int64_t next_location_ = 720001;
  std::int64_t next_core_data_point_ = 730001;
  std::int64_t next_data_point_ = 740001;
  std::int64_t next_aliquot_ = 750001;
  std::int64_t next_measurement_ = 760001;
  std::int64_t next_age_calc_ = 770001;
  std::int64_t next_age_summary_ = 780001;
};

}  // namespace pychron::earthbank::testing
