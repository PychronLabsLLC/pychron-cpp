#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fake_earthbank.hpp"
#include "ooxml.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/vocab.hpp"

namespace pychron::earthbank {
namespace {

using namespace std::string_view_literals;
using nlohmann::json;
using testing::FakeEarthBank;
using testing::LoggedRequest;
using testing::Store;

constexpr std::string_view kNotWritable = "Package is not writable by user.";

// A program talking to the fake: it logs in once and sends what a client of
// the service sends, the JSON content type on every body.
class Client {
 public:
  explicit Client(FakeEarthBank& bank, bool logged_in = true) : bank_(bank) {
    if (logged_in) token_ = bank.issue_token();
  }

  HttpRequest request(std::string method, std::string_view path_and_query, const json* body = nullptr) const {
    HttpRequest out;
    out.method = std::move(method);
    out.url = bank_.base_url() + std::string(path_and_query);
    if (!token_.empty()) out.headers.emplace_back("Authorization", "Bearer " + token_);
    if (body != nullptr) {
      out.headers.emplace_back("Content-Type", "application/json");
      out.body = body->dump();
    }
    return out;
  }

  HttpResponse get(std::string_view path_and_query) const { return answered(bank_.send(request("GET", path_and_query))); }
  HttpResponse post(std::string_view path, const json& body) const {
    return answered(bank_.send(request("POST", path, &body)));
  }
  HttpResponse del(std::string_view path) const { return answered(bank_.send(request("DELETE", path))); }

  void set_token(std::string token) { token_ = std::move(token); }
  const std::string& token() const { return token_; }

 private:
  static HttpResponse answered(Result<HttpResponse> result) {
    if (!result) throw std::runtime_error("no answer: " + to_string(result.error()));
    return *result;
  }

  FakeEarthBank& bank_;
  std::string token_;
};

json parsed(const HttpResponse& response) {
  json out = json::parse(response.body, nullptr, false);
  if (out.is_discarded()) throw std::runtime_error("not JSON: " + response.body);
  return out;
}

std::vector<std::string> names(const HttpResponse& response) {
  std::vector<std::string> out;
  for (const json& row : parsed(response)) out.push_back(row.at("name").get<std::string>());
  return out;
}

std::int64_t term(const FakeEarthBank& bank, std::string_view endpoint, std::string_view name) {
  const auto id = bank.lookup_id(endpoint, name);
  if (!id) throw std::runtime_error("fixture has no " + std::string(name) + " at " + std::string(endpoint));
  return *id;
}

// The sample wrapper as legacy posts it: the package on the wrapper and on
// the sample both.
json sample_body(std::int64_t package, const std::string& name) {
  return {{"sampleDTO", {{"name", name}, {"sampleID", name}, {"dataPackageId", package}}},
          {"locationDTO", {{"lat", -34.93}, {"lon", 138.6}, {"name", name}}},
          {"dataPackageId", package},
          {"shortName", name}};
}

std::int64_t new_sample(const Client& client, std::int64_t package, const std::string& name) {
  const HttpResponse response = client.post("/api/core/sample-with-locations", sample_body(package, name));
  if (response.status != 201) throw std::runtime_error("sample refused: " + response.body);
  return parsed(response).at("sampleDTO").at("id").get<std::int64_t>();
}

json data_point_fields(const FakeEarthBank& bank) {
  return {{"analysisDate", "2026-05-20"},
          {"analysisUnits", "fA"},
          {"arMethodName", "Step-heating - laser"},
          {"arMethodId", term(bank, "/api/arar/LArArMethod", "Step-heating - laser")},
          {"mineralName", "sanidine"},
          {"mineralId", term(bank, "/api/core/materials", "Sanidine")},
          {"analyticalUncertaintyTypeId", 1}};
}

json data_point_body(const FakeEarthBank& bank, std::int64_t package, std::int64_t sample, const std::string& name) {
  return {{"dataPointDTO",
           {{"dataStructure", "ARARDATAPOINT"}, {"dataPackageId", package}, {"sampleId", sample}, {"name", name}}},
          {"extendingDataPointDTO", data_point_fields(bank)}};
}

std::int64_t new_data_point(const Client& client, const FakeEarthBank& bank, std::int64_t package,
                            const std::string& name) {
  const std::int64_t sample = new_sample(client, package, name);
  const HttpResponse response = client.post("/api/arar/ArArDataPoint", data_point_body(bank, package, sample, name));
  if (response.status != 201) throw std::runtime_error("data point refused: " + response.body);
  return parsed(response).at("id").get<std::int64_t>();
}

std::string printed(const std::vector<LoggedRequest>& log) {
  std::ostringstream out;
  out << ::testing::PrintToString(log);
  for (const LoggedRequest& one : log) out << one << '\n';
  return out.str();
}

// ---- Session ---------------------------------------------------------------

TEST(FakeEarthBank, Authenticate) {
  FakeEarthBank bank;
  const Client anonymous(bank, false);

  const HttpResponse ok = anonymous.post(
      "/api/authenticate", {{"username", bank.login()}, {"password", bank.password()}, {"rememberMe", false}});
  ASSERT_EQ(ok.status, 200) << ok.body;
  const std::string token = parsed(ok).at("id_token").get<std::string>();
  EXPECT_FALSE(token.empty());

  EXPECT_EQ(anonymous.post("/api/authenticate", {{"username", bank.login()}, {"password", "wrong"}}).status, 401);
  EXPECT_EQ(anonymous.post("/api/authenticate", {{"username", "nobody"}, {"password", bank.password()}}).status, 401);
  EXPECT_EQ(anonymous.post("/api/authenticate", json::object()).status, 401);

  // The account is the connection test and names the user.
  EXPECT_EQ(anonymous.get("/api/account").status, 401);
  Client client(bank, false);
  client.set_token(token);
  const HttpResponse account = client.get("/api/account");
  ASSERT_EQ(account.status, 200) << account.body;
  EXPECT_EQ(parsed(account).at("login"), bank.login());
  client.set_token("not-a-token");
  EXPECT_EQ(client.get("/api/account").status, 401);

  // Two logins, two tokens, both good.
  const std::string second = parsed(anonymous.post("/api/authenticate",
                                                   {{"username", bank.login()}, {"password", bank.password()}}))
                                 .at("id_token")
                                 .get<std::string>();
  EXPECT_NE(second, token);
  client.set_token(token);
  EXPECT_EQ(client.get("/api/account").status, 200);
  client.set_token(second);
  EXPECT_EQ(client.get("/api/account").status, 200);
}

TEST(FakeEarthBank, ExpiredTokensAreRefusedUntilANewLogin) {
  FakeEarthBank bank;
  Client client(bank);
  ASSERT_EQ(client.get("/api/account").status, 200);

  bank.expire_tokens();
  EXPECT_EQ(client.get("/api/account").status, 401);
  EXPECT_EQ(client.get("/api/account").status, 401) << "a void token stays void";

  const HttpResponse again =
      client.post("/api/authenticate", {{"username", bank.login()}, {"password", bank.password()}});
  ASSERT_EQ(again.status, 200);
  client.set_token(parsed(again).at("id_token").get<std::string>());
  EXPECT_EQ(client.get("/api/account").status, 200);
}

// Review Focus 4: the password is changed, or was wrong, in the middle of a run.
TEST(FakeEarthBank, AChangedPasswordVoidsTheSession) {
  FakeEarthBank bank;
  Client client(bank);
  const std::string old_password = bank.password();
  bank.set_password("another-not-real-password");
  EXPECT_EQ(client.get("/api/account").status, 401);
  EXPECT_EQ(client.post("/api/authenticate", {{"username", bank.login()}, {"password", old_password}}).status, 401);
  EXPECT_EQ(client.post("/api/authenticate", {{"username", bank.login()}, {"password", bank.password()}}).status,
            200);
}

// A body with no JSON content type is not read (legacy sets it on every POST).
TEST(FakeEarthBank, ABodyNeedsTheJsonContentType) {
  FakeEarthBank bank;
  const Client client(bank);
  HttpRequest request = client.request("POST", "/api/authenticate");
  request.body = json{{"username", bank.login()}, {"password", bank.password()}}.dump();
  auto bare = bank.send(request);
  ASSERT_TRUE(bare.has_value());
  EXPECT_EQ(bare->status, 415);
  request.headers.emplace_back("content-type", "application/json; charset=utf-8");
  auto typed = bank.send(request);
  ASSERT_TRUE(typed.has_value());
  EXPECT_EQ(typed->status, 200);

  EXPECT_EQ(client.post("/api/arar/ArArAliquot", json::array({1, 2})).status, 400);
  HttpRequest broken = client.request("POST", "/api/arar/ArArAliquot");
  broken.headers.emplace_back("Content-Type", "application/json");
  broken.body = "{\"aliquotName\":";
  auto half = bank.send(broken);
  ASSERT_TRUE(half.has_value());
  EXPECT_EQ(half->status, 400);
}

TEST(FakeEarthBank, WhatIsNotThereIs404) {
  FakeEarthBank bank;
  const Client client(bank);
  EXPECT_EQ(client.get("/api/arar/NoSuchThing").status, 404);
  EXPECT_EQ(client.get("/apiv2/account").status, 404);
  EXPECT_EQ(client.get("/api/management/data-packages/1").status, 404);
  EXPECT_EQ(client.del("/api/account").status, 405);
  EXPECT_EQ(client.post("/api/arar/FluxMonitor", json::object()).status, 405);
}

// A request to another host is not one to the service.
TEST(FakeEarthBank, AnotherHostIsNeverReached) {
  FakeEarthBank bank;
  HttpRequest request;
  request.method = "GET";
  for (const char* url : {"https://elsewhere.test/api/account", "https://earthbank.test.evil.test/api/account",
                          "http://earthbank.test/api/account", "/api/account"}) {
    request.url = url;
    const auto result = bank.send(request);
    ASSERT_FALSE(result.has_value()) << url;
    EXPECT_EQ(result.error().kind, ErrorKind::Io) << url;
    EXPECT_FALSE(request_may_have_arrived(result.error())) << url;
  }
  EXPECT_TRUE(bank.requests().empty());

  FakeEarthBank::Options options;
  options.base_url = "http://127.0.0.1:8080";
  FakeEarthBank local(options);
  request.url = "http://127.0.0.1:8080/api/arar/AirRatio";
  const auto result = local.send(request);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, 200);
}

// ---- Lookups ---------------------------------------------------------------

TEST(FakeEarthBank, LookupEquals) {
  FakeEarthBank bank;
  const Client client(bank, false);

  const HttpResponse one = client.get(with_query("/api/arar/LAnalysisScale", {{"name.equals", "Single Grain"}}));
  ASSERT_EQ(one.status, 200) << one.body;
  const json rows = parsed(one);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("name"), "Single Grain");
  EXPECT_EQ(rows[0].at("id").get<std::int64_t>(), term(bank, "/api/arar/LAnalysisScale", "Single Grain"));

  // Exact, and the case counts: `sanidine` is not `Sanidine`.
  EXPECT_EQ(names(client.get(with_query("/api/core/materials", {{"name.equals", "Sanidine"}}))),
            std::vector<std::string>{"Sanidine"});
  EXPECT_TRUE(parsed(client.get(with_query("/api/core/materials", {{"name.equals", "sanidine"}}))).empty());
  EXPECT_TRUE(parsed(client.get(with_query("/api/core/materials", {{"name.equals", "Sanidin"}}))).empty());
  EXPECT_TRUE(parsed(client.get(with_query("/api/arar/LAnalysisScale", {{"name.equals", "No Such Scale"}}))).empty());

  // Names that need encoding arrive whole (Review Focus 2).
  for (const std::string name : {"Arizonite (of Spurr & Washington)", "Julgoldite-(Fe2+)-Pumpellyite-(Mg) Series",
                                 "Coarse-grained (\"plutonic\") crystalline igneous rock", "Chabourn\xC3\xA9ite Group",
                                 "Pele's hair"}) {
    EXPECT_EQ(names(client.get(with_query("/api/core/materials", {{"name.equals", name}}))),
              std::vector<std::string>{name});
  }
  EXPECT_EQ(names(client.get(with_query("/api/arar/LUncertaintyUnit", {{"name.equals", "%"}}))),
            std::vector<std::string>{"%"});

  // The service's entry ends in a space, as the vendor's list has it: the
  // name without it is not equal, and is found by `contains`.
  EXPECT_TRUE(parsed(client.get(with_query("/api/arar/DecayConstant", {{"name.equals", "Renne et al. 2011"}}))).empty());
  EXPECT_EQ(names(client.get(with_query("/api/arar/DecayConstant", {{"name.contains", "Renne et al. 2011"}}))),
            std::vector<std::string>{"Renne et al. 2011 "});
}

TEST(FakeEarthBank, LookupContainsPaged) {
  FakeEarthBank bank;
  const Client client(bank, false);

  // Without case, anywhere in the name.
  const std::vector<std::string> all =
      names(client.get(with_query("/api/core/materials", {{"name.contains", "biotite"}, {"size", "200"}})));
  ASSERT_GT(all.size(), 20u) << "the fixture is to hold more biotites than a page";
  for (const std::string& name : all) {
    std::string lower = name;
    for (char& c : lower) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    EXPECT_NE(lower.find("biotite"), std::string::npos) << name;
  }
  EXPECT_EQ(names(client.get(with_query("/api/core/materials", {{"name.contains", "BIOTITE"}, {"size", "200"}}))),
            all);

  // A page is 20 rows unless asked otherwise, and pages count from 0.
  const auto first = names(client.get(with_query("/api/core/materials", {{"name.contains", "biotite"}})));
  ASSERT_EQ(first.size(), 20u);
  EXPECT_EQ(first, std::vector<std::string>(all.begin(), all.begin() + 20));
  const auto second = names(client.get(with_query("/api/core/materials", {{"name.contains", "biotite"}, {"page", "1"}})));
  EXPECT_EQ(second, std::vector<std::string>(all.begin() + 20, all.end()));
  EXPECT_TRUE(
      parsed(client.get(with_query("/api/core/materials", {{"name.contains", "biotite"}, {"page", "2"}}))).empty());

  const auto threes =
      names(client.get(with_query("/api/core/materials", {{"name.contains", "biotite"}, {"page", "1"}, {"size", "3"}})));
  EXPECT_EQ(threes, std::vector<std::string>(all.begin() + 3, all.begin() + 6));
  EXPECT_EQ(parsed(client.get(with_query("/api/core/materials", {{"size", "1"}}))).size(), 1u);

  // The whole of a short vocabulary in one page, as legacy loads it.
  EXPECT_EQ(parsed(client.get(with_query("/api/arar/FluxMonitor", {{"size", "2000"}}))).size(), 16u);
  // Both filters at once: both hold.
  EXPECT_TRUE(parsed(client.get(with_query("/api/core/materials",
                                           {{"name.contains", "biotite"}, {"name.equals", "Sanidine"}})))
                  .empty());
}

TEST(FakeEarthBank, LookupNeedsABearerOnCoreEndpoints) {
  FakeEarthBank bank;
  const Client anonymous(bank, false);
  const Client client(bank);

  int with_login_only = 0, open = 0;
  std::set<std::string_view> seen;
  for (const LookupSpec& spec : lookup_specs()) {
    if (spec.endpoint.empty() || !seen.insert(spec.endpoint).second) continue;
    const std::string url = with_query(spec.endpoint, {{"size", "2000"}});
    const bool core_list = spec.endpoint.starts_with("/api/core/l-");
    EXPECT_EQ(anonymous.get(url).status, core_list ? 401 : 200) << spec.endpoint;
    EXPECT_EQ(client.get(url).status, 200) << spec.endpoint;
    ++(core_list ? with_login_only : open);
  }
  EXPECT_EQ(with_login_only, 3);  // sample kinds, sample methods, location kinds
  EXPECT_EQ(open, 14);

  // A token that is no good is as none: the open lists stay open.
  Client stale(bank, false);
  stale.set_token("not-a-token");
  EXPECT_EQ(stale.get("/api/arar/AirRatio").status, 200);
  EXPECT_EQ(stale.get("/api/core/l-sample-kinds").status, 401);
}

// Every `*Name` field the library resolves has somewhere to be resolved, and
// an id means one thing only.
TEST(FakeEarthBank, EveryLookupEndpointIsServed) {
  FakeEarthBank bank;
  const Client client(bank);
  std::set<std::int64_t> ids;
  std::set<std::string_view> seen;
  for (const LookupSpec& spec : lookup_specs()) {
    if (spec.endpoint.empty() || !seen.insert(spec.endpoint).second) continue;
    const HttpResponse response = client.get(with_query(spec.endpoint, {{"size", "2000"}}));
    ASSERT_EQ(response.status, 200) << spec.endpoint;
    const json rows = parsed(response);
    ASSERT_TRUE(rows.is_array());
    EXPECT_FALSE(rows.empty()) << spec.endpoint;
    for (const json& row : rows) {
      ASSERT_TRUE(row.at("id").is_number_integer()) << spec.endpoint;
      ASSERT_TRUE(row.at("name").is_string()) << spec.endpoint;
      const auto id = row.at("id").get<std::int64_t>();
      EXPECT_TRUE(ids.insert(id).second) << id << " is in two vocabularies, or twice in " << spec.endpoint;
      EXPECT_GT(id, 2) << "1 and 2 are the uncertainty types";
    }
  }
  EXPECT_EQ(seen.size(), 17u);
}

// The lists the two routes share are the same lists: what the workbook's
// template offers is what the fake's service knows, entry for entry.
TEST(FakeEarthBank, TheArArListsAreTheTemplates) {
  FakeEarthBank bank;
  const Client client(bank);
  auto book = ooxml::Workbook::open(std::filesystem::path(PYCHRON_EARTHBANK_TEMPLATE_DIR) /
                                    "ArArDataPoint_Template2026.xlsx");
  ASSERT_TRUE(book.has_value()) << to_string(book.error());
  struct List {
    const char* endpoint;
    int column, last_row;
  };
  for (const List& list : {List{"/api/arar/DecayConstant", 1, 5}, List{"/api/arar/AirRatio", 2, 3},
                           List{"/api/arar/FluxMonitor", 3, 17}, List{"/api/arar/LArArMethod", 4, 13},
                           List{"/api/arar/LAnalysisScale", 5, 8}, List{"/api/arar/LUncertaintyUnit", 7, 3},
                           List{"/api/arar/LAgeType", 8, 9}, List{"/api/arar/LArArDataInterpretationTool", 10, 11},
                           List{"/api/arar/LArArInterpretation", 11, 20}}) {
    const auto column = book->column_values("Lookup Tables", list.column, 2, list.last_row);
    ASSERT_TRUE(column.has_value()) << list.endpoint;
    EXPECT_EQ(names(client.get(with_query(list.endpoint, {{"size", "2000"}}))), *column) << list.endpoint;
  }
  // The names the mapping writes are among them.
  EXPECT_TRUE(bank.lookup_id("/api/arar/LAgeType", age_type_name(AgeKind::Plateau)).has_value());
  EXPECT_TRUE(bank.lookup_id("/api/arar/LArArMethod", ar_method(true, 2.0)).has_value());
  EXPECT_TRUE(bank.lookup_id("/api/arar/LUncertaintyUnit", kAbsoluteUncertainty).has_value());
  EXPECT_TRUE(bank.lookup_id("/api/arar/LArArInterpretation", kUnknownInterpretation).has_value());
  EXPECT_TRUE(bank.lookup_id("/api/arar/LArArDataInterpretationTool", kInterpretationTool).has_value());
  EXPECT_TRUE(bank.lookup_id("/api/arar/AirRatio", air_ratio_name(298.56)).has_value());
  EXPECT_TRUE(bank.lookup_id("/api/arar/DecayConstant", decay_constant_name(5.463e-10)).has_value());
}

// The list that looks like the uncertainty types and is not: its ids are not
// the ones a record takes (legacy's comment on LOOKUP_ENDPOINTS).
TEST(FakeEarthBank, TheErrorTypesListIsATrap) {
  FakeEarthBank bank;
  const Client client(bank);
  const HttpResponse response = client.get(with_query("/api/core/l-error-types", {{"name.equals", "1 sigma"}}));
  ASSERT_EQ(response.status, 200);
  const json rows = parsed(response);
  ASSERT_EQ(rows.size(), 1u);
  const auto wrong = rows[0].at("id").get<std::int64_t>();
  EXPECT_NE(wrong, 1);

  const std::int64_t package = bank.writable_package();
  json body = data_point_body(bank, package, new_sample(client, package, "S-TRAP"), "S-TRAP");
  body["extendingDataPointDTO"]["analyticalUncertaintyTypeId"] = wrong;
  const HttpResponse refused = client.post("/api/arar/ArArDataPoint", body);
  EXPECT_EQ(refused.status, 400);
  EXPECT_NE(refused.body.find("analytical_uncertainty_type_id"), std::string::npos) << refused.body;
}

TEST(FakeEarthBank, ARowCanBeAddedToAVocabulary) {
  FakeEarthBank bank;
  const Client client(bank, false);
  bank.add_lookup_row("/api/arar/FluxMonitor", {{"id", 1999}, {"name", "A monitor"}, {"reference", "Somebody 2020"}});
  const json rows = parsed(client.get(with_query("/api/arar/FluxMonitor", {{"name.equals", "A monitor"}})));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("reference"), "Somebody 2020");
  EXPECT_EQ(bank.lookup_id("/api/arar/FluxMonitor", "A monitor"), 1999);
  EXPECT_THROW(bank.add_lookup_row("/api/arar/NoSuchList", {{"id", 1}, {"name", "x"}}), std::runtime_error);
  EXPECT_THROW(bank.add_lookup_row("/api/arar/FluxMonitor", {{"name", "no id"}}), std::runtime_error);
}

// ---- Packages --------------------------------------------------------------

TEST(FakeEarthBank, PackagesAreListedByInstitutionAndName) {
  FakeEarthBank bank;
  const Client client(bank);
  EXPECT_EQ(Client(bank, false).get("/api/management/data-packages").status, 401);

  const json all = parsed(client.get("/api/management/data-packages?page=0&size=500&sort=name,ASC"));
  ASSERT_GE(all.size(), 3u);
  std::vector<std::string> listed;
  std::set<std::int64_t> institutions;
  for (const json& row : all) {
    // The row is a wrapper: the package's fields are under dataPackageDTO.
    const json& dto = row.at("dataPackageDTO");
    listed.push_back(dto.at("name").get<std::string>());
    institutions.insert(dto.at("institutionId").get<std::int64_t>());
    EXPECT_TRUE(dto.at("id").is_number_integer());
    EXPECT_TRUE(row.contains("countSupervisors"));
    // Nothing in a row says whether the account can write to it.
    EXPECT_FALSE(row.dump().find("ritable") != std::string::npos) << row.dump();
  }
  std::vector<std::string> sorted = listed;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(listed, sorted);
  ASSERT_GE(institutions.size(), 2u);

  const std::int64_t institution = *institutions.begin();
  const json some = parsed(client.get(with_query(
      "/api/management/data-packages", {{"institutionId.equals", std::to_string(institution)}, {"size", "500"}})));
  EXPECT_LT(some.size(), all.size());
  EXPECT_FALSE(some.empty());
  for (const json& row : some) EXPECT_EQ(row.at("dataPackageDTO").at("institutionId"), institution);

  const json archive =
      parsed(client.get(with_query("/api/management/data-packages", {{"name.contains", "ARCHIVE"}})));
  ASSERT_EQ(archive.size(), 1u);

  // One package by its id.
  const HttpResponse one = client.get("/api/management/data-packages/" + std::to_string(bank.writable_package()));
  ASSERT_EQ(one.status, 200);
  EXPECT_EQ(parsed(one).at("dataPackageDTO").at("id"), bank.writable_package());
  EXPECT_EQ(parsed(one).at("countSamples"), 0);
}

// The trap legacy documents (create_data_package): the fields go under
// dataPackageDTO; a flat body is accepted and the name is lost.
TEST(FakeEarthBank, PackageFlatBodyIgnoresTheName) {
  FakeEarthBank bank;
  const Client client(bank);
  const json fields = {{"name", "My new package"},
                       {"distribution", "PRIVATE"},
                       {"workflowState", "IN_PROGRESS"},
                       {"licenseText", "Creative Commons"},
                       {"institutionId", 9301}};

  const HttpResponse flat = client.post("/api/management/data-packages", fields);
  ASSERT_EQ(flat.status, 201) << flat.body;
  const json flat_dto = parsed(flat).at("dataPackageDTO");
  EXPECT_NE(flat_dto.at("name"), "My new package");
  EXPECT_FALSE(flat_dto.at("name").get<std::string>().empty());

  json nested = fields;  // as legacy sends it: nested, and mirrored at the top
  nested["dataPackageDTO"] = fields;
  const HttpResponse good = client.post("/api/management/data-packages", nested);
  ASSERT_EQ(good.status, 201) << good.body;
  const json dto = parsed(good).at("dataPackageDTO");
  EXPECT_EQ(dto.at("name"), "My new package");
  EXPECT_EQ(dto.at("institutionId"), 9301);
  EXPECT_EQ(dto.at("distribution"), "PRIVATE");
  EXPECT_NE(dto.at("id"), flat_dto.at("id"));

  // Both are there, and the creator can write to both.
  const auto id = dto.at("id").get<std::int64_t>();
  EXPECT_TRUE(bank.can_write(id));
  EXPECT_TRUE(bank.can_write(flat_dto.at("id").get<std::int64_t>()));
  EXPECT_EQ(new_sample(client, id, "S-NEW"), parsed(client.get(with_query("/api/core/sample-with-locations",
                                                                            {{"name.equals", "S-NEW"}})))[0]
                                                 .at("sampleDTO")
                                                 .at("id"));
  const json listed = parsed(client.get(with_query("/api/management/data-packages", {{"name.equals", "My new package"}})));
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed[0].at("countSamples"), 1);

  // Who made it is not the account's id (legacy README, known limitations):
  // a client cannot tell its own packages by comparing the two.
  const auto account_id = parsed(client.get("/api/account")).at("id").get<std::int64_t>();
  EXPECT_NE(dto.at("createdById").get<std::int64_t>(), account_id);
}

TEST(FakeEarthBank, APackageIsDeletedWithWhatIsInIt) {
  FakeEarthBank bank;
  const Client client(bank);
  const json fields = {{"name", "Throwaway"}};
  const auto id = parsed(client.post("/api/management/data-packages", {{"dataPackageDTO", fields}}))
                      .at("dataPackageDTO")
                      .at("id")
                      .get<std::int64_t>();
  new_data_point(client, bank, id, "S-GONE");
  ASSERT_EQ(bank.store().samples.size(), 1u);
  ASSERT_EQ(bank.store().data_points.size(), 1u);

  const std::string path = "/api/management/data-packages/" + std::to_string(id);
  EXPECT_EQ(client.del("/api/management/data-packages/" + std::to_string(bank.read_only_package())).status, 500);
  EXPECT_EQ(client.del(path).status, 204);
  EXPECT_EQ(client.get(path).status, 404);
  EXPECT_EQ(client.del(path).status, 404);
  EXPECT_TRUE(bank.store().samples.empty());
  EXPECT_TRUE(bank.store().data_points.empty());
}

// ---- Samples ---------------------------------------------------------------

// Legacy's find_sample_by_name and its scope test: a sample is in one
// package, and a search for a package's sample says so.
TEST(FakeEarthBank, SampleSearchIsScopedToThePackage) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t here = bank.writable_package();
  const std::int64_t there = bank.add_package("Another writable package", true);

  const std::int64_t theirs = new_sample(client, there, "S1");
  auto search = [&](const std::string& name, std::optional<std::int64_t> package) {
    std::string url = with_query("/api/core/sample-with-locations", {{"name.equals", name}, {"size", "5"}});
    if (package) url = with_query(url, {{"dataPackageId.equals", std::to_string(*package)}});
    const HttpResponse response = client.get(url);
    EXPECT_EQ(response.status, 200) << response.body;
    return parsed(response);
  };

  EXPECT_TRUE(search("S1", here).empty()) << "the sample of that name is in another package";
  ASSERT_EQ(search("S1", there).size(), 1u);
  EXPECT_EQ(search("S1", there)[0].at("sampleDTO").at("id"), theirs);

  const std::int64_t ours = new_sample(client, here, "S1");
  EXPECT_NE(ours, theirs);
  const json found = search("S1", here);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].at("sampleDTO").at("id"), ours);
  EXPECT_EQ(found[0].at("sampleDTO").at("name"), "S1");
  EXPECT_EQ(found[0].at("sampleDTO").at("dataPackageId"), here);
  EXPECT_DOUBLE_EQ(found[0].at("locationDTO").at("lat").get<double>(), -34.93);

  // Without the package the search is of everything.
  EXPECT_EQ(search("S1", std::nullopt).size(), 2u);
  EXPECT_TRUE(search("S", here).empty()) << "equals is the whole name";
  EXPECT_TRUE(search("s1", here).empty());

  // A name that needs encoding.
  const std::string odd = "S 1/\xC3\xA9 & <2> 50%+\"q\"";
  const std::int64_t odd_id = new_sample(client, here, odd);
  ASSERT_EQ(search(odd, here).size(), 1u);
  EXPECT_EQ(search(odd, here)[0].at("sampleDTO").at("id"), odd_id);

  EXPECT_EQ(Client(bank, false).get("/api/core/sample-with-locations?name.equals=S1").status, 401);
}

// The package goes on the wrapper and on the sample: legacy stamps both, not
// knowing which the service reads, and that is the only form known to work.
TEST(FakeEarthBank, ASampleNeedsItsPackageInBothPlaces) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const char* const path = "/api/core/sample-with-locations";

  json no_wrapper = sample_body(package, "S1");
  no_wrapper.erase("dataPackageId");
  json no_inner = sample_body(package, "S1");
  no_inner["sampleDTO"].erase("dataPackageId");
  json disagree = sample_body(package, "S1");
  disagree["dataPackageId"] = bank.add_package("Another", true);
  const json flat = {{"name", "S1"}, {"dataPackageId", package}};
  for (const json& body : {no_wrapper, no_inner, disagree, flat}) {
    const HttpResponse response = client.post(path, body);
    EXPECT_EQ(response.status, 500) << body.dump();
    EXPECT_EQ(parsed(response).at("title").get<std::string>(), kNotWritable);
  }
  EXPECT_TRUE(bank.store().samples.empty());

  // What comes back: the ids are in the wrapper's parts, not on top of it.
  const HttpResponse made = client.post(path, sample_body(package, "S1"));
  ASSERT_EQ(made.status, 201) << made.body;
  const json row = parsed(made);
  EXPECT_FALSE(row.contains("id"));
  const auto id = row.at("sampleDTO").at("id").get<std::int64_t>();
  EXPECT_EQ(row.at("sampleDTO").at("locationId"), row.at("locationDTO").at("id"));
  const Store store = bank.store();
  ASSERT_EQ(store.samples.size(), 1u);
  EXPECT_EQ(store.samples[0].id, id);
  EXPECT_EQ(store.samples[0].package_id, package);
  EXPECT_EQ(store.samples[0].body.at("shortName"), "S1");

  // A sample with no place: legacy sends an empty locationDTO.
  json nowhere = sample_body(package, "S2");
  nowhere["locationDTO"] = json::object();
  const json placed = parsed(client.post(path, nowhere));
  EXPECT_FALSE(placed.at("sampleDTO").contains("locationId"));
  EXPECT_FALSE(placed.at("locationDTO").contains("id"));

  EXPECT_EQ(Client(bank, false).post(path, sample_body(package, "S3")).status, 401);
}

// ---- The data point and what hangs from it ---------------------------------

// Legacy's README and arar_post_probe: the Ar-Ar fields alone, wherever the
// package is put among them, are "not writable"; the package is read from the
// umbrella record of the wrapper.
TEST(FakeEarthBank, DataPointNeedsTheWrapper) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const std::int64_t sample = new_sample(client, package, "S1");
  const char* const path = "/api/arar/ArArDataPoint";

  json flat = data_point_fields(bank);
  json with_int = flat, with_text = flat, with_other_key = flat, nested = flat;
  with_int["dataPackageId"] = package;
  with_text["dataPackageId"] = std::to_string(package);
  with_other_key["packageId"] = package;
  nested["dataPackage"] = {{"id", package}};
  for (const json& body : {flat, with_int, with_text, with_other_key, nested}) {
    const HttpResponse response = client.post(path, body);
    EXPECT_EQ(response.status, 500) << body.dump();
    EXPECT_EQ(response.body, R"({"title":"Package is not writable by user."})");
  }
  EXPECT_EQ(client.post(with_query(path, {{"dataPackageId", std::to_string(package)}}), flat).status, 500);
  EXPECT_TRUE(bank.store().data_points.empty());

  json no_package = data_point_body(bank, package, sample, "S1 sanidine");
  no_package["dataPointDTO"].erase("dataPackageId");
  EXPECT_EQ(client.post(path, no_package).status, 500);

  const HttpResponse made = client.post(path, data_point_body(bank, package, sample, "S1 sanidine"));
  ASSERT_EQ(made.status, 201) << made.body;
  const json row = parsed(made);
  // The id on top is the arArDataPointId; the umbrella record has its own.
  const auto id = row.at("id").get<std::int64_t>();
  EXPECT_EQ(row.at("extendingDataPointDTO").at("id"), id);
  EXPECT_NE(row.at("dataPointDTO").at("id"), id);
  EXPECT_EQ(row.at("dataPointDTO").at("sampleId"), sample);
  EXPECT_NE(id, sample);

  const Store store = bank.store();
  ASSERT_EQ(store.data_points.size(), 1u);
  EXPECT_EQ(store.data_points[0].id, id);
  EXPECT_EQ(store.data_points[0].package_id, package);
  EXPECT_EQ(store.data_points[0].body.at("dataPointDTO").at("name"), "S1 sanidine");
  EXPECT_EQ(store.data_points[0].body.at("extendingDataPointDTO").at("analysisUnits"), "fA");
  EXPECT_EQ(parsed(client.get("/api/management/data-packages/" + std::to_string(package))).at("countArArDataPoints"), 1);

  // The set-up and the irradiation go beside the two records, by name.
  json named = data_point_body(bank, package, sample, "S1 biotite");
  named["arArSetUpName"] = "Example Lab ARGUS VI";
  named["arArIrradiationName"] = "EX-12";
  ASSERT_EQ(client.post(path, named).status, 201);
  EXPECT_EQ(bank.store().data_points[1].body.at("arArSetUpName"), "Example Lab ARGUS VI");

  EXPECT_EQ(Client(bank, false).post(path, data_point_body(bank, package, sample, "S1")).status, 401);
}

TEST(FakeEarthBank, SubRecordsHangFromTheDataPoint) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const std::int64_t point = new_data_point(client, bank, package, "S1");

  const HttpResponse aliquot =
      client.post("/api/arar/ArArAliquot", {{"aliquotName", "12345-01"}, {"arArDataPointId", point}});
  ASSERT_EQ(aliquot.status, 201) << aliquot.body;
  EXPECT_EQ(parsed(aliquot).at("aliquotName"), "12345-01");
  const HttpResponse measurement = client.post(
      "/api/arar/ArArMeasurement",
      {{"aliquotName", "12345-01"}, {"arArDataPointId", point}, {"apparentAge", 28.2}, {"plateauStepName", "Yes"},
       {"apparentAgeUncertainityTypeId", 1}});
  ASSERT_EQ(measurement.status, 201) << measurement.body;
  const HttpResponse calc = client.post(
      "/api/arar/ArArAgeCalc",
      {{"arArDataPointId", point},
       {"decayConstantName", "Min et al. 2000"},
       {"decayConstantId", term(bank, "/api/arar/DecayConstant", "Min et al. 2000")},
       {"fluxMonitorId",
        term(bank, "/api/arar/FluxMonitor", "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)")}});
  ASSERT_EQ(calc.status, 201) << calc.body;
  const HttpResponse summary = client.post(
      "/api/arar/ArArAgeSummary",
      {{"arArDataPointId", point},
       {"age", 28.2},
       {"ageCalcTypeId", term(bank, "/api/arar/LAgeType", "Plateau Age")},
       {"ageUncertaintyTypeId", 1},
       {"preferredAge", true}});
  ASSERT_EQ(summary.status, 201) << summary.body;

  // Four kinds, four ids, none of them the data point's.
  std::set<std::int64_t> ids = {point};
  for (const HttpResponse* response : {&aliquot, &measurement, &calc, &summary}) {
    EXPECT_TRUE(ids.insert(parsed(*response).at("id").get<std::int64_t>()).second);
  }
  const Store store = bank.store();
  ASSERT_EQ(store.aliquots.size(), 1u);
  ASSERT_EQ(store.measurements.size(), 1u);
  ASSERT_EQ(store.age_calcs.size(), 1u);
  ASSERT_EQ(store.age_summaries.size(), 1u);
  EXPECT_EQ(store.measurements[0].package_id, package);
  EXPECT_EQ(store.measurements[0].body.at("apparentAge"), 28.2);
  EXPECT_EQ(store.measurements[0].body.at("plateauStepName"), "Yes");
  EXPECT_EQ(store.age_summaries[0].body.at("preferredAge"), true);

  // The service does not know one record from another like it: posted twice,
  // it is there twice. Nothing is updated and nothing is refused as a copy.
  ASSERT_EQ(client.post("/api/arar/ArArAliquot", {{"aliquotName", "12345-01"}, {"arArDataPointId", point}}).status,
            201);
  EXPECT_EQ(bank.store().aliquots.size(), 2u);

  // No data point, or one that is not there: no package to write to.
  for (const json& body : {json{{"aliquotName", "x"}}, json{{"aliquotName", "x"}, {"arArDataPointId", point + 999}},
                           json{{"aliquotName", "x"}, {"arArDataPointId", store.samples[0].id}}}) {
    const HttpResponse response = client.post("/api/arar/ArArAliquot", body);
    EXPECT_EQ(response.status, 500) << body.dump();
    EXPECT_EQ(parsed(response).at("title").get<std::string>(), kNotWritable);
  }
  EXPECT_EQ(Client(bank, false).post("/api/arar/ArArAliquot", {{"arArDataPointId", point}}).status, 401);
}

// A `*Id` that is not a row of that field's vocabulary: the database refuses
// the record, and the answer names the column (legacy's _constraint_field
// reads it out of exactly this).
TEST(FakeEarthBank, WrongVocabularyIdIsAConstraintError) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const std::int64_t sample = new_sample(client, package, "S1");

  json body = data_point_body(bank, package, sample, "S1");
  body["extendingDataPointDTO"]["mineralId"] = 424242;
  const HttpResponse response = client.post("/api/arar/ArArDataPoint", body);
  ASSERT_EQ(response.status, 400) << response.body;
  const std::string detail = parsed(response).at("detail").get<std::string>();
  EXPECT_NE(detail.find("constraint"), std::string::npos) << detail;
  EXPECT_NE(detail.find("Key (mineral_id)=(424242) is not present in table"), std::string::npos) << detail;
  EXPECT_NE(detail.find("constraint \"fk_ar_ar_data_point_mineral_id\""), std::string::npos) << detail;
  EXPECT_TRUE(bank.store().data_points.empty());

  // An id of another vocabulary is as wrong as one of none.
  body = data_point_body(bank, package, sample, "S1");
  body["extendingDataPointDTO"]["arMethodId"] = term(bank, "/api/arar/LAnalysisScale", "Single Grain");
  EXPECT_NE(client.post("/api/arar/ArArDataPoint", body).body.find("Key (ar_method_id)"), std::string::npos);

  // So in every record that has such a field, the sample's included.
  const std::int64_t point = new_data_point(client, bank, package, "S2");
  const std::int64_t scale = term(bank, "/api/arar/LAnalysisScale", "Single Grain");
  struct Case {
    const char* path;
    const char* field;
    const char* column;
  };
  for (const Case& c : {Case{"/api/arar/ArArAgeCalc", "fluxMonitorId", "flux_monitor_id"},
                        Case{"/api/arar/ArArAgeCalc", "decayConstantId", "decay_constant_id"},
                        Case{"/api/arar/ArArAgeCalc", "airRatioId", "air_ratio_id"},
                        Case{"/api/arar/ArArAgeSummary", "ageCalcTypeId", "age_calc_type_id"},
                        Case{"/api/arar/ArArAgeSummary", "interpretationId", "interpretation_id"},
                        Case{"/api/arar/ArArAgeSummary", "calculatedAgeUncertaintyTypeUnitsId",
                             "calculated_age_uncertainty_type_units_id"}}) {
    const HttpResponse refused = client.post(c.path, {{"arArDataPointId", point}, {c.field, scale}});
    EXPECT_EQ(refused.status, 400) << c.field;
    EXPECT_NE(refused.body.find(std::string("Key (") + c.column + ")"), std::string::npos) << refused.body;
  }
  json bad_sample = sample_body(package, "S3");
  bad_sample["sampleDTO"]["materialId"] = scale;
  const HttpResponse refused = client.post("/api/core/sample-with-locations", bad_sample);
  EXPECT_EQ(refused.status, 400);
  EXPECT_NE(refused.body.find("Key (material_id)"), std::string::npos) << refused.body;
  const Store store = bank.store();
  EXPECT_TRUE(store.age_calcs.empty());
  EXPECT_TRUE(store.age_summaries.empty());
  EXPECT_EQ(store.samples.size(), 2u);
}

// The sigma level of an uncertainty: 1 and 2, the ids of a table with no
// endpoint (legacy's UNCERTAINTY_TYPE_VOCAB), in every field that has one.
TEST(FakeEarthBank, UncertaintyTypeIdsAreOneAndTwo) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t point = new_data_point(client, bank, bank.writable_package(), "S1");
  EXPECT_EQ(client.post("/api/arar/ArArMeasurement", {{"arArDataPointId", point}, {"jvalueUncertaintyTypeId", 2}}).status,
            201);
  const HttpResponse refused =
      client.post("/api/arar/ArArMeasurement", {{"arArDataPointId", point}, {"apparentAgeUncertainityTypeId", 3}});
  EXPECT_EQ(refused.status, 400);
  // The service's own misspelling, in its column as in its field.
  EXPECT_NE(refused.body.find("Key (apparent_age_uncertainity_type_id)=(3)"), std::string::npos) << refused.body;
  EXPECT_EQ(client.post("/api/arar/ArArAgeSummary",
                        {{"arArDataPointId", point},
                         {"ageUncertaintyTypeId", term(bank, "/api/arar/LUncertaintyUnit", "abs.")}})
                .status,
            400);
}

// Legacy drops a name it could not resolve "so the server doesn't 400 on the
// row": a term's name with no id beside it is refused. With its id it is
// carried along, and the id is what counts.
TEST(FakeEarthBank, ATermsNameWithoutItsIdIsRefused) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const std::int64_t sample = new_sample(client, package, "S1");

  json body = data_point_body(bank, package, sample, "S1");
  body["extendingDataPointDTO"].erase("mineralId");
  const HttpResponse refused = client.post("/api/arar/ArArDataPoint", body);
  EXPECT_EQ(refused.status, 400) << refused.body;
  EXPECT_NE(refused.body.find("mineralName"), std::string::npos) << refused.body;
  EXPECT_TRUE(bank.store().data_points.empty());

  // Not a term the service lists: free text, an enumeration, a sigma level.
  body = data_point_body(bank, package, sample, "S1");
  body["extendingDataPointDTO"]["lithologyName"] = "Rhyolite";
  body["extendingDataPointDTO"]["analyticalUncertaintyTypeName"] = "1 sigma";
  EXPECT_EQ(client.post("/api/arar/ArArDataPoint", body).status, 201);
}

TEST(FakeEarthBank, UnwritablePackageIsRefused) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t locked = bank.read_only_package();
  ASSERT_FALSE(bank.can_write(locked));

  const HttpResponse sample = client.post("/api/core/sample-with-locations", sample_body(locked, "S1"));
  EXPECT_EQ(sample.status, 500);
  EXPECT_EQ(sample.body, R"({"title":"Package is not writable by user."})");
  // A package that is not there at all is no more writable.
  EXPECT_EQ(client.post("/api/core/sample-with-locations", sample_body(555, "S1")).status, 500);

  const std::int64_t open = bank.writable_package();
  const std::int64_t ours = new_sample(client, open, "S1");
  const HttpResponse point = client.post("/api/arar/ArArDataPoint", data_point_body(bank, locked, ours, "S1"));
  EXPECT_EQ(point.status, 500);
  EXPECT_EQ(parsed(point).at("title").get<std::string>(), kNotWritable);

  // Permission taken away in the middle of an upload: what hangs from the
  // data point is refused from then on.
  const std::int64_t id = new_data_point(client, bank, open, "S2");
  ASSERT_EQ(client.post("/api/arar/ArArAliquot", {{"aliquotName", "a"}, {"arArDataPointId", id}}).status, 201);
  bank.set_writable(open, false);
  const HttpResponse late = client.post("/api/arar/ArArAliquot", {{"aliquotName", "b"}, {"arArDataPointId", id}});
  EXPECT_EQ(late.status, 500);
  EXPECT_EQ(parsed(late).at("title").get<std::string>(), kNotWritable);
  EXPECT_EQ(bank.store().aliquots.size(), 1u);
  // Reading is not writing.
  EXPECT_EQ(client.get(with_query("/api/core/sample-with-locations",
                                  {{"name.equals", "S1"}, {"dataPackageId.equals", std::to_string(open)}}))
                .status,
            200);
}

// What the package scope of the sample search is there to prevent (legacy,
// find_sample_by_name): the service links a data point to a sample of another
// package without a word, and the target package is left with no sample.
TEST(FakeEarthBank, ASampleOfAnotherPackageIsLinkedWithoutAWord) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t here = bank.writable_package();
  const std::int64_t there = bank.add_package("Another writable package", true);
  const std::int64_t theirs = new_sample(client, there, "S1");

  ASSERT_EQ(client.post("/api/arar/ArArDataPoint", data_point_body(bank, here, theirs, "S1")).status, 201);
  const json row = parsed(client.get("/api/management/data-packages/" + std::to_string(here)));
  EXPECT_EQ(row.at("countArArDataPoints"), 1);
  EXPECT_EQ(row.at("countSamples"), 0);

  // A sample that does not exist is another matter.
  const HttpResponse none = client.post("/api/arar/ArArDataPoint", data_point_body(bank, here, 12345, "S2"));
  EXPECT_EQ(none.status, 400);
  EXPECT_NE(none.body.find("Key (sample_id)=(12345)"), std::string::npos) << none.body;
}

// ---- Failures on demand ----------------------------------------------------

// Review Focus 1. The request is carried out and the answer never comes: the
// client knows only that it does not know.
TEST(FakeEarthBank, DropNextAppliesAndTimesOut) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const std::int64_t sample = new_sample(client, package, "S1");
  const json body = data_point_body(bank, package, sample, "S1");

  bank.drop_next("POST", "/api/arar/ArArDataPoint");
  const auto lost = bank.send(client.request("POST", "/api/arar/ArArDataPoint", &body));
  ASSERT_FALSE(lost.has_value());
  EXPECT_EQ(lost.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(request_may_have_arrived(lost.error()));
  ASSERT_EQ(bank.store().data_points.size(), 1u) << "the record is there all the same";
  const std::int64_t written = bank.store().data_points[0].id;

  const std::vector<LoggedRequest> log = bank.requests();
  EXPECT_EQ(log.back().outcome, LoggedRequest::Outcome::Dropped);
  EXPECT_EQ(log.back().status, 201);

  // Once only. And a client that simply tries again has made a second record.
  const HttpResponse again = client.post("/api/arar/ArArDataPoint", body);
  ASSERT_EQ(again.status, 201);
  EXPECT_NE(parsed(again).at("id"), written);
  EXPECT_EQ(bank.store().data_points.size(), 2u);

  // The sample is the one record that can be asked for afterwards.
  bank.drop_next("POST", "/api/core/sample-with-locations");
  const json lost_sample = sample_body(package, "S-LOST");
  ASSERT_FALSE(bank.send(client.request("POST", "/api/core/sample-with-locations", &lost_sample)).has_value());
  const json found = parsed(client.get(with_query(
      "/api/core/sample-with-locations", {{"name.equals", "S-LOST"}, {"dataPackageId.equals", std::to_string(package)}})));
  ASSERT_EQ(found.size(), 1u);

  // A refusal that is dropped is still nothing written.
  bank.drop_next("POST", "/api/arar/ArArAliquot");
  const json orphan = {{"aliquotName", "x"}};
  const auto refused = bank.send(client.request("POST", "/api/arar/ArArAliquot", &orphan));
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(bank.store().aliquots.empty());
  EXPECT_EQ(bank.requests().back().status, 500);
}

TEST(FakeEarthBank, FailNextAnswersOnceAndWritesNothing) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t point = new_data_point(client, bank, bank.writable_package(), "S1");
  const json aliquot = {{"aliquotName", "a"}, {"arArDataPointId", point}};

  bank.fail_next("POST", "/api/arar/ArArAliquot", 503, "<html>Service Unavailable</html>");
  bank.fail_next("POST", "/api/arar/", 502, "");
  // Another method, another path: not theirs.
  EXPECT_EQ(client.get("/api/arar/AirRatio").status, 200);
  EXPECT_EQ(client.post("/api/core/sample-with-locations", sample_body(bank.writable_package(), "S2")).status, 201);

  const HttpResponse first = client.post("/api/arar/ArArAliquot", aliquot);
  EXPECT_EQ(first.status, 503);
  EXPECT_EQ(first.body, "<html>Service Unavailable</html>");
  EXPECT_EQ(client.post("/api/arar/ArArMeasurement", {{"arArDataPointId", point}}).status, 502);
  EXPECT_TRUE(bank.store().aliquots.empty());
  EXPECT_TRUE(bank.store().measurements.empty());
  EXPECT_EQ(client.post("/api/arar/ArArAliquot", aliquot).status, 201);
  EXPECT_EQ(bank.count("POST", "/api/arar/ArArAliquot"), 2u);
  EXPECT_EQ(bank.count("POST", "/api/arar/"), 4u);
  EXPECT_EQ(bank.count("GET", "/api/arar/ArArAliquot"), 0u);

  // The login can be made to fail too, and a 401 can be put on anything.
  bank.fail_next("POST", "/api/authenticate", 500, "{}");
  EXPECT_EQ(client.post("/api/authenticate", {{"username", bank.login()}, {"password", bank.password()}}).status, 500);
  bank.fail_next("GET", "/api/account", 401, "");
  EXPECT_EQ(client.get("/api/account").status, 401);
  EXPECT_EQ(client.get("/api/account").status, 200);
}

TEST(FakeEarthBank, RefuseNextNeverArrives) {
  FakeEarthBank bank;
  const Client client(bank);
  const json body = sample_body(bank.writable_package(), "S1");
  bank.refuse_next("POST", "/api/core/");
  const auto result = bank.send(client.request("POST", "/api/core/sample-with-locations", &body));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::Io);
  EXPECT_EQ(result.error().code, kHttpNotSent);
  EXPECT_FALSE(request_may_have_arrived(result.error()));
  EXPECT_TRUE(bank.store().samples.empty());
  EXPECT_EQ(bank.requests().back().outcome, LoggedRequest::Outcome::NotSent);
  EXPECT_EQ(bank.requests().back().status, 0);
  EXPECT_EQ(client.post("/api/core/sample-with-locations", body).status, 201);
}

// The other half of an answer that never came: the request was not carried
// out either. The client cannot tell this from drop_next, and that is the
// point: both are "in doubt", and only one of them left a record.
TEST(FakeEarthBank, LoseNextWritesNothingAndGivesNoAnswer) {
  FakeEarthBank bank;
  const Client client(bank);
  const json body = sample_body(bank.writable_package(), "S1");

  bank.lose_next("POST", "/api/core/sample-with-locations");
  const auto timed_out = bank.send(client.request("POST", "/api/core/sample-with-locations", &body));
  ASSERT_FALSE(timed_out.has_value());
  EXPECT_EQ(timed_out.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(timed_out.error().code.empty());
  EXPECT_TRUE(request_may_have_arrived(timed_out.error()));
  EXPECT_TRUE(bank.store().samples.empty());
  EXPECT_EQ(bank.requests().back().outcome, LoggedRequest::Outcome::Lost);
  EXPECT_EQ(bank.requests().back().status, 0);

  bank.lose_next("POST", "/api/core/sample-with-locations", ErrorKind::Io);
  const auto reset = bank.send(client.request("POST", "/api/core/sample-with-locations", &body));
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(reset.error().kind, ErrorKind::Io);
  EXPECT_NE(reset.error().code, kHttpNotSent);
  EXPECT_TRUE(request_may_have_arrived(reset.error()));
  EXPECT_TRUE(bank.store().samples.empty());

  // Once each.
  EXPECT_EQ(client.post("/api/core/sample-with-locations", body).status, 201);
  // Only the two ways an answer is lost: anything else is a mistake in the test.
  EXPECT_THROW(bank.lose_next("POST", "/api/", ErrorKind::Config), std::invalid_argument);
  EXPECT_THROW(bank.drop_next("POST", "/api/", ErrorKind::Cancelled), std::invalid_argument);
  EXPECT_TRUE(bank.pending().empty());
}

// The record is made and the connection goes before the answer: an I/O
// error, and not one that says the request never left.
TEST(FakeEarthBank, DropNextCanLoseTheConnection) {
  FakeEarthBank bank;
  const Client client(bank);
  const json body = sample_body(bank.writable_package(), "S1");
  bank.drop_next("POST", "/api/core/", ErrorKind::Io);
  const auto lost = bank.send(client.request("POST", "/api/core/sample-with-locations", &body));
  ASSERT_FALSE(lost.has_value());
  EXPECT_EQ(lost.error().kind, ErrorKind::Io);
  EXPECT_NE(lost.error().code, kHttpNotSent);
  EXPECT_TRUE(request_may_have_arrived(lost.error()));
  EXPECT_EQ(bank.store().samples.size(), 1u);
  EXPECT_EQ(bank.requests().back().outcome, LoggedRequest::Outcome::Dropped);
  EXPECT_EQ(bank.requests().back().status, 201);
}

// A script can wait: `skip` requests that fit go by untouched first. And
// what has not fired can be asked for, so a test can say its script was used.
TEST(FakeEarthBank, AScriptCanWaitForALaterRequest) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t point = new_data_point(client, bank, bank.writable_package(), "S1");
  auto aliquot = [&](const char* name) {
    const json body = {{"aliquotName", name}, {"arArDataPointId", point}};
    return bank.send(client.request("POST", "/api/arar/ArArAliquot", &body));
  };

  bank.fail_next("POST", "/api/arar/ArArAliquot", 503, "busy", 2);
  bank.drop_next("POST", "/api/arar/ArArAliquot", ErrorKind::Timeout, 3);
  bank.lose_next("POST", "/api/arar/ArArAliquot", ErrorKind::Io, 4);
  bank.refuse_next("POST", "/api/arar/ArArAliquot", 5);
  using Pending = testing::PendingScript;
  EXPECT_EQ(bank.pending(), (std::vector<Pending>{{"fail", "POST", "/api/arar/ArArAliquot", 2},
                                                  {"drop", "POST", "/api/arar/ArArAliquot", 3},
                                                  {"lose", "POST", "/api/arar/ArArAliquot", 4},
                                                  {"refuse", "POST", "/api/arar/ArArAliquot", 5}}));

  // A request that does not fit is not counted.
  EXPECT_EQ(client.get("/api/arar/AirRatio").status, 200);
  EXPECT_EQ(aliquot("a")->status, 201);
  EXPECT_EQ(aliquot("b")->status, 201);
  EXPECT_EQ(bank.pending().front().skip, 0u);
  EXPECT_EQ(aliquot("c")->status, 503);            // the third
  ASSERT_EQ(bank.pending().size(), 3u);
  // Every script counts the requests that fit it, whichever of them fires:
  // the fourth is the drop's, the fifth the lose's, the sixth the refuse's.
  const auto fourth = aliquot("d");
  ASSERT_FALSE(fourth.has_value());
  EXPECT_EQ(fourth.error().kind, ErrorKind::Timeout);
  const auto fifth = aliquot("e");
  ASSERT_FALSE(fifth.has_value());
  EXPECT_EQ(fifth.error().kind, ErrorKind::Io);
  EXPECT_TRUE(request_may_have_arrived(fifth.error()));
  const auto sixth = aliquot("f");
  ASSERT_FALSE(sixth.has_value());
  EXPECT_FALSE(request_may_have_arrived(sixth.error()));
  EXPECT_TRUE(bank.pending().empty());
  EXPECT_EQ(aliquot("g")->status, 201);

  std::vector<std::string> made;
  // cppcheck-suppress danglingTempReference ; the loop's reference to a member keeps the whole store alive
  for (const Store::Record& record : bank.store().aliquots) made.push_back(record.body.at("aliquotName"));
  EXPECT_EQ(made, (std::vector<std::string>{"a", "b", "d", "g"}));

  // A script nothing fitted is still there at the end.
  bank.fail_next("DELETE", "/api/management/", 500, "");
  ASSERT_EQ(bank.pending().size(), 1u);
  EXPECT_EQ(bank.pending()[0].kind, "fail");
  EXPECT_EQ(bank.pending()[0].method, "DELETE");
}

// ---- Behaviour that is not known, to be had either way ---------------------

// Legacy does not trust the service to keep to `dataPackageId.equals`: it
// looks at each row's package itself (find_sample_by_name). A client that
// does not is shown the sample of another package here.
TEST(FakeEarthBank, TheSampleSearchCanIgnoreThePackage) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t here = bank.writable_package();
  const std::int64_t there = bank.add_package("Another writable package", true);
  const std::int64_t theirs = new_sample(client, there, "S1");
  const std::string url = with_query("/api/core/sample-with-locations",
                                     {{"name.equals", "S1"}, {"dataPackageId.equals", std::to_string(here)}});
  ASSERT_TRUE(parsed(client.get(url)).empty());

  bank.ignore_sample_package_filter(true);
  const json rows = parsed(client.get(url));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("sampleDTO").at("id"), theirs);
  EXPECT_EQ(rows[0].at("sampleDTO").at("dataPackageId"), there) << "the row still says where it is";
  // The name is still minded.
  EXPECT_TRUE(parsed(client.get(with_query("/api/core/sample-with-locations",
                                           {{"name.equals", "S2"}, {"dataPackageId.equals", std::to_string(here)}})))
                  .empty());

  bank.ignore_sample_package_filter(false);
  EXPECT_TRUE(parsed(client.get(url)).empty());
}

// Legacy looks for the word "constraint" in the body before it looks at the
// status, so what status the service gives is not known.
TEST(FakeEarthBank, AConstraintFailureCanBeA500) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t point = new_data_point(client, bank, bank.writable_package(), "S1");
  const json body = {{"arArDataPointId", point}, {"fluxMonitorId", 424242}};
  EXPECT_EQ(client.post("/api/arar/ArArAgeCalc", body).status, 400);

  bank.constraint_status(500);
  const HttpResponse refused = client.post("/api/arar/ArArAgeCalc", body);
  EXPECT_EQ(refused.status, 500);
  EXPECT_EQ(parsed(refused).at("status"), 500);
  EXPECT_NE(refused.body.find("Key (flux_monitor_id)=(424242)"), std::string::npos) << refused.body;
  EXPECT_EQ(refused.body.find("not writable"), std::string::npos);
  // The other refusals keep their status.
  EXPECT_EQ(client.post("/api/arar/ArArAgeCalc", {{"arArDataPointId", point}, {"decayConstantName", "x"}}).status, 400);
  EXPECT_THROW(bank.constraint_status(200), std::invalid_argument);
}

// Legacy takes any 2xx for a record made.
TEST(FakeEarthBank, WhatIsCreatedCanAnswer200) {
  FakeEarthBank bank;
  const Client client(bank);
  bank.created_status(200);
  const std::int64_t package = bank.writable_package();
  const HttpResponse sample = client.post("/api/core/sample-with-locations", sample_body(package, "S1"));
  ASSERT_EQ(sample.status, 200);
  const auto sample_id = parsed(sample).at("sampleDTO").at("id").get<std::int64_t>();
  const HttpResponse point = client.post("/api/arar/ArArDataPoint", data_point_body(bank, package, sample_id, "S1"));
  ASSERT_EQ(point.status, 200);
  const json aliquot = {{"arArDataPointId", parsed(point).at("id")}};
  EXPECT_EQ(client.post("/api/arar/ArArAliquot", aliquot).status, 200);
  const json fields = {{"name", "P"}};
  EXPECT_EQ(client.post("/api/management/data-packages", {{"dataPackageDTO", fields}}).status, 200);
  EXPECT_EQ(bank.store().aliquots.size(), 1u);

  bank.created_status(201);
  EXPECT_EQ(client.post("/api/arar/ArArAliquot", aliquot).status, 201);
  for (const int status : {199, 204, 300, 404}) EXPECT_THROW(bank.created_status(status), std::invalid_argument);
}

// Where the id of a new sample is in the answer: legacy reads `sampleDTO.id`,
// then `sample.id` (_extract_sample_id), and other records' ids on top.
TEST(FakeEarthBank, ANewSamplesIdCanBeElsewhereInTheAnswer) {
  using Place = testing::SampleIdPlace;
  FakeEarthBank bank;
  const Client client(bank);
  const std::int64_t package = bank.writable_package();
  const char* const path = "/api/core/sample-with-locations";

  bank.sample_id_place(Place::Sample);
  const json under_sample = parsed(client.post(path, sample_body(package, "S1")));
  EXPECT_FALSE(under_sample.contains("sampleDTO"));
  EXPECT_FALSE(under_sample.contains("id"));
  const auto first = under_sample.at("sample").at("id").get<std::int64_t>();
  EXPECT_EQ(under_sample.at("sample").at("name"), "S1");

  bank.sample_id_place(Place::Top);
  const json on_top = parsed(client.post(path, sample_body(package, "S2")));
  const auto second = on_top.at("id").get<std::int64_t>();
  EXPECT_FALSE(on_top.at("sampleDTO").contains("id"));
  EXPECT_EQ(on_top.at("sampleDTO").at("name"), "S2");

  bank.sample_id_place(Place::SampleDto);
  const json usual = parsed(client.post(path, sample_body(package, "S3")));
  EXPECT_FALSE(usual.contains("id"));
  EXPECT_EQ(usual.at("sampleDTO").at("id").get<std::int64_t>(), second + 1);
  EXPECT_EQ(second, first + 1);

  // Only the answer to the POST differs: what is kept, and what a search
  // gives, is the same sample either way.
  const Store store = bank.store();
  ASSERT_EQ(store.samples.size(), 3u);
  EXPECT_EQ(store.samples[0].id, first);
  EXPECT_EQ(store.samples[1].body.at("sampleDTO").at("id"), second);
  EXPECT_EQ(parsed(client.get(with_query(path, {{"name.equals", "S1"}})))[0].at("sampleDTO").at("id"), first);
  ASSERT_EQ(client.post("/api/arar/ArArDataPoint", data_point_body(bank, package, second, "S2")).status, 201);
}

// ---- The log ---------------------------------------------------------------

TEST(FakeEarthBank, TheLogSaysWhatWasAsked) {
  FakeEarthBank bank;
  const Client client(bank);
  ASSERT_EQ(client.get(with_query("/api/core/materials", {{"name.contains", "Spurr & Washington"}, {"size", "200"}}))
                .status,
            200);
  ASSERT_EQ(Client(bank, false).get("/api/account").status, 401);
  const std::vector<LoggedRequest> log = bank.requests();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0].method, "GET");
  EXPECT_EQ(log[0].path, "/api/core/materials");
  EXPECT_EQ(log[0].param("name.contains"), "Spurr & Washington");
  EXPECT_EQ(log[0].param("size"), "200");
  EXPECT_EQ(log[0].param("page"), std::nullopt);
  EXPECT_EQ(log[0].status, 200);
  EXPECT_TRUE(log[0].authenticated);
  EXPECT_EQ(log[0].outcome, LoggedRequest::Outcome::Answered);
  EXPECT_EQ(log[1].status, 401);
  EXPECT_FALSE(log[1].authenticated);

  std::ostringstream line;
  line << log[0];
  EXPECT_EQ(line.str().rfind("GET /api/core/materials?name.contains=Spurr & Washington&size=200 -> 200", 0), 0u)
      << line.str();
}

// No password and no token is in the log, however it is printed: a failing
// assertion on requests() shows the whole of it.
TEST(FakeEarthBank, TheLogHoldsNoSecret) {
  FakeEarthBank::Options options;
  options.password = "PASSWORD-s3cret";
  FakeEarthBank bank(options);
  Client client(bank, false);

  const HttpResponse login =
      client.post("/api/authenticate", {{"username", bank.login()}, {"password", "PASSWORD-s3cret"}});
  ASSERT_EQ(login.status, 200);
  const std::string token = parsed(login).at("id_token").get<std::string>();
  client.set_token(token);
  ASSERT_EQ(client.get("/api/account").status, 200);
  ASSERT_EQ(client.post("/api/authenticate", {{"username", bank.login()}, {"password", "WRONG-s3cret"}}).status, 401);

  // A login that is not JSON, a token in a body, a credential in other headers.
  HttpRequest odd = client.request("POST", "/api/authenticate");
  odd.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
                 {"authorization", "Bearer " + token},
                 {"Cookie", "session=COOKIE-s3cret"},
                 {"X-Auth-Token", "HEADER-s3cret"},
                 {"Proxy-Authorization", "Basic PROXY-s3cret"}};
  odd.body = "username=x&password=FORM-s3cret";
  ASSERT_TRUE(bank.send(odd).has_value());
  const json nested = {{"aliquotName", "a"}, {"nested", {{"id_token", token}, {"Password", "NESTED-s3cret"}}}};
  client.post("/api/arar/ArArAliquot", nested);
  bank.drop_next("GET", "/api/account");
  ASSERT_FALSE(bank.send(client.request("GET", "/api/account")).has_value());

  const std::vector<LoggedRequest> log = bank.requests();
  ASSERT_EQ(log.size(), 6u);
  const std::string text = printed(log);
  for (const std::string& secret : {std::string("s3cret"), token, std::string("PASSWORD"), std::string("WRONG")}) {
    EXPECT_EQ(text.find(secret), std::string::npos) << secret << " in\n" << text;
    for (const LoggedRequest& one : log) {
      EXPECT_EQ(one.body.find(secret), std::string::npos) << one.body;
      for (const auto& [name, value] : one.headers) EXPECT_EQ(value.find(secret), std::string::npos) << name;
    }
  }
  // What is not a secret is still there to be asserted on.
  EXPECT_NE(text.find("POST /api/authenticate -> 200"), std::string::npos) << text;
  EXPECT_NE(text.find("<redacted>"), std::string::npos);
  EXPECT_EQ(json::parse(log[0].body).at("username"), bank.login());
  EXPECT_EQ(json::parse(log[4].body).at("aliquotName"), "a");
  EXPECT_TRUE(log[1].authenticated);

  // Nor does an error carry one.
  bank.drop_next("POST", "/api/authenticate");
  const json again = {{"username", bank.login()}, {"password", "PASSWORD-s3cret"}};
  const auto lost = bank.send(client.request("POST", "/api/authenticate", &again));
  ASSERT_FALSE(lost.has_value());
  EXPECT_EQ(to_string(lost.error()).find("s3cret"), std::string::npos);
  EXPECT_EQ(to_string(lost.error()).find(token), std::string::npos);
}

// A credential in a query is no more to be kept than one in a header.
TEST(FakeEarthBank, TheLogHoldsNoSecretOfAQuery) {
  FakeEarthBank bank;
  const Client client(bank);
  const std::string url = with_query("/api/core/materials", {{"name.equals", "Sanidine"},
                                                             {"access_token", "QUERY-s3cret-1"},
                                                             {"id_token", "QUERY-s3cret-2"},
                                                             {"Password", "QUERY-s3cret-3"},
                                                             {"token", "QUERY-s3cret-4"},
                                                             {"api_key", "QUERY-s3cret-5"},
                                                             {"client_secret", "QUERY-s3cret-6"}});
  ASSERT_EQ(names(client.get(url)), std::vector<std::string>{"Sanidine"});
  const std::vector<LoggedRequest> log = bank.requests();
  ASSERT_EQ(log.size(), 1u);
  EXPECT_EQ(printed(log).find("s3cret"), std::string::npos) << printed(log);
  EXPECT_EQ(log[0].param("name.equals"), "Sanidine");
  for (const char* key : {"access_token", "id_token", "Password", "token", "api_key", "client_secret"}) {
    EXPECT_EQ(log[0].param(key), "<redacted>") << key;
  }
}

// ---- The assumptions -------------------------------------------------------

std::string file_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The whole numbers in `text`, e.g. of "(2, 18)".
std::vector<int> numbers_in(std::string_view text) {
  std::vector<int> out;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] < '0' || text[i] > '9') {
      ++i;
      continue;
    }
    int value = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) value = value * 10 + (text[i] - '0');
    out.push_back(value);
  }
  return out;
}

// What the fake does without legacy's word for it is marked in its source,
// `// assumed (n):`, and listed for the owner in the fixtures' README, item
// n. The two are one list: no mark without its item, no item without a mark.
TEST(FakeEarthBank, EveryAssumptionIsMarkedAndListed) {
  const std::filesystem::path fixtures = PYCHRON_EARTHBANK_FIXTURE_DIR;
  const std::string source = file_text(fixtures.parent_path() / "fake_earthbank.cpp");
  const std::string readme = file_text(fixtures / "README.md");

  std::set<int> marked;
  std::size_t marks = 0;
  for (std::size_t at = source.find("// assumed"); at != std::string::npos; at = source.find("// assumed", at + 1)) {
    const std::size_t open = at + std::string_view("// assumed").size();
    const std::size_t close = source.find("):", open);
    ASSERT_EQ(source.compare(open, 2, " ("), 0) << "a mark with no number: " << source.substr(at, 60);
    ASSERT_NE(close, std::string::npos);
    const std::vector<int> items = numbers_in(std::string_view(source).substr(open, close - open));
    ASSERT_FALSE(items.empty()) << source.substr(at, 60);
    marked.insert(items.begin(), items.end());
    ++marks;
  }
  EXPECT_GT(marks, 0u);
  EXPECT_EQ(source.find("assumed:"), std::string::npos) << "an assumption marked without its number";

  // The README's items: lines `<n>. ` after the heading, numbered from 1 on.
  const std::size_t heading = readme.find("## Assumptions to check against the real service");
  ASSERT_NE(heading, std::string::npos);
  std::set<int> listed;
  std::istringstream lines(readme.substr(heading));
  for (std::string line; std::getline(lines, line);) {
    const std::size_t dot = line.find(". ");
    if (dot == std::string::npos || dot == 0 || dot > 3) continue;
    if (line.find_first_not_of("0123456789") != dot) continue;
    const int item = numbers_in(line.substr(0, dot)).front();
    EXPECT_TRUE(listed.insert(item).second) << "item " << item << " is there twice";
    EXPECT_EQ(item, static_cast<int>(listed.size())) << "the items are numbered in order";
  }
  EXPECT_GE(listed.size(), 29u);
  EXPECT_EQ(marked, listed);
}

// ---- Determinism -----------------------------------------------------------

TEST(FakeEarthBank, TheSameCallsGiveTheSameIds) {
  auto run = [] {
    FakeEarthBank bank;
    const Client client(bank);
    std::vector<std::int64_t> ids;
    const json fields = {{"name", "Run"}};
    const auto package = parsed(client.post("/api/management/data-packages", {{"dataPackageDTO", fields}}))
                             .at("dataPackageDTO")
                             .at("id")
                             .get<std::int64_t>();
    ids.push_back(package);
    ids.push_back(bank.add_package("Another", true));
    for (const char* name : {"S1", "S2"}) {
      const std::int64_t point = new_data_point(client, bank, package, name);
      ids.push_back(point);
      for (const char* path : {"/api/arar/ArArAliquot", "/api/arar/ArArMeasurement", "/api/arar/ArArAgeCalc",
                               "/api/arar/ArArAgeSummary"}) {
        ids.push_back(parsed(client.post(path, {{"arArDataPointId", point}})).at("id").get<std::int64_t>());
      }
    }
    // cppcheck-suppress danglingTempReference ; the loop's reference to a member keeps the whole store alive
    for (const Store::Record& sample : bank.store().samples) ids.push_back(sample.id);
    return std::make_pair(ids, client.token());
  };
  const auto first = run();
  const auto second = run();
  EXPECT_EQ(first, second);
  // And no id is given twice, whatever the kind of record.
  EXPECT_EQ(std::set<std::int64_t>(first.first.begin(), first.first.end()).size(), first.first.size());
}

TEST(FakeEarthBank, AMissingFixtureIsNamed) {
  FakeEarthBank::Options options;
  options.fixtures = std::filesystem::path(PYCHRON_EARTHBANK_FIXTURE_DIR) / "no-such-directory";
  try {
    FakeEarthBank bank(options);
    FAIL() << "built from nothing";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("no-such-directory"), std::string::npos) << error.what();
  }
}

}  // namespace
}  // namespace pychron::earthbank
