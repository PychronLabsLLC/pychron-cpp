// What ships for the lab's box (packaging/observability) against what the
// application exports: a metric renamed in the code and not on its dashboard,
// or the other way round, fails here.
#ifdef PYCHRON_EXPERIMENT_HAS_METRICS

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/metrics_config.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/metrics/service.hpp"

#ifdef PYCHRON_TEST_HAS_JSON
#include <nlohmann/json.hpp>
#endif

namespace fs = std::filesystem;
using namespace pychron;

namespace {

const fs::path kRoot = fs::path(PYCHRON_SOURCE_DIR) / "packaging" / "observability";

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<fs::path> files_under(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!fs::exists(dir)) return out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.is_regular_file()) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<fs::path> dashboards() {
  std::vector<fs::path> out;
  for (const fs::path& p : files_under(kRoot / "grafana" / "dashboards")) {
    if (p.extension() == ".json") out.push_back(p);
  }
  return out;
}

// Every family the application registers when `[metrics]` is on.
std::set<std::string> exported() {
  ManualClock clock;
  SignalBus bus;
  Scheduler scheduler(clock, &bus, Scheduler::Options{0});
  config::MetricsConfig c;
  c.enabled = true;
  c.bind = "127.0.0.1";
  c.port = 0;
  auto service = experiment::metrics::MetricsService::start(c, bus, scheduler, clock, nullptr, "0.0.0");
  const std::vector<std::string> names = service->registry().names();
  return {names.begin(), names.end()};
}

// The metric a query token names: a histogram's _bucket, _sum and _count are
// its family's.
std::string family_of(const std::string& token, const std::set<std::string>& known) {
  if (known.contains(token)) return token;
  for (const char* suffix : {"_bucket", "_sum", "_count"}) {
    const std::string s(suffix);
    if (token.size() > s.size() && token.ends_with(s)) {
      const std::string base = token.substr(0, token.size() - s.size());
      if (known.contains(base)) return base;
    }
  }
  return token;
}

// file -> the families its queries name.
std::map<std::string, std::set<std::string>> used(const std::set<std::string>& known) {
  static const std::regex name("pychron_[a-z0-9_]+");
  std::map<std::string, std::set<std::string>> out;
  for (const fs::path& p : files_under(kRoot)) {
    const std::string text = slurp(p);
    for (std::sregex_iterator it(text.begin(), text.end(), name), end; it != end; ++it) {
      out[p.filename().string()].insert(family_of(it->str(), known));
    }
  }
  return out;
}

}  // namespace

TEST(MetricsPackaging, TheFilesAreThere) {
  for (const char* rel :
       {"prometheus/pychron.scrape.yml", "grafana/provisioning/dashboards/pychron.yml",
        "grafana/provisioning/alerting/pychron-deadman.yml", "grafana/dashboards/instrument-health.json",
        "grafana/dashboards/run-operations.json", "grafana/dashboards/app-health.json"}) {
    EXPECT_TRUE(fs::exists(kRoot / rel)) << rel;
  }
}

TEST(MetricsPackaging, QueriesUseOnlyMetricsThatExist) {
  const std::set<std::string> known = exported();
  const auto by_file = used(known);
  ASSERT_FALSE(by_file.empty()) << "no file under " << kRoot << " names a metric";
  for (const auto& [file, names] : by_file) {
    for (const std::string& n : names) EXPECT_EQ(known.count(n), 1u) << file << " uses " << n << ", which nothing exports";
  }
}

TEST(MetricsPackaging, EveryExportedMetricIsOnSomeDashboard) {
  const std::set<std::string> known = exported();
  std::set<std::string> shown;
  for (const auto& [file, names] : used(known)) {
    if (file.size() > 5 && file.ends_with(".json")) shown.insert(names.begin(), names.end());
  }
  // The endpoint's own bookkeeping is for whoever debugs the endpoint.
  const std::set<std::string> exempt{"pychron_metrics_dropped_series_total", "pychron_metrics_bad_requests_total"};
  for (const std::string& n : known) {
    if (exempt.contains(n)) continue;
    EXPECT_EQ(shown.count(n), 1u) << n << " is exported and on no dashboard";
  }
}

TEST(MetricsPackaging, TheAlertNamesBothConditions) {
  const std::string alert = slurp(kRoot / "grafana" / "provisioning" / "alerting" / "pychron-deadman.yml");
  EXPECT_NE(alert.find("last_over_time(pychron_queue_active[24h])"), std::string::npos);
  EXPECT_NE(alert.find("up{job=\\\"pychron\\\"} == 0"), std::string::npos);
  EXPECT_NE(alert.find("pychron_scheduler_heartbeat_age_seconds > 60"), std::string::npos);
  // The box's clock and the instrument computer's need not agree: nothing here compares them.
  EXPECT_EQ(alert.find("time()"), std::string::npos);
  // Grafana leaves a template's dollar alone and keeps a doubled one as it is
  // (tried on 12.1 with packaging/observability/box): one dollar.
  EXPECT_NE(alert.find("{{ $labels.instrument }}"), std::string::npos);
  EXPECT_EQ(alert.find("$$"), std::string::npos);
  EXPECT_NE(alert.find("for: 2m"), std::string::npos);
}

TEST(MetricsPackaging, NoDashboardComparesTheBoxsClockWithTheInstruments) {
  for (const fs::path& p : dashboards()) {
    EXPECT_EQ(slurp(p).find("time()"), std::string::npos) << p.filename();
  }
}

// Two series that differ in a label only divide when the query says to
// ignore it; without that the panel is silently empty.
TEST(MetricsPackaging, QueueProgressDividesDoneByTotal) {
  const std::string board = slurp(kRoot / "grafana" / "dashboards" / "run-operations.json");
  EXPECT_NE(board.find("status=\\\"done\\\"} / ignoring(status) clamp_min(pychron_queue_runs{"), std::string::npos);
}

// The virtual box (box/docker-compose.yml) loads the real box's files; what
// it adds of its own must agree with them.
TEST(MetricsPackaging, TheVirtualBoxAgreesWithTheRealOnesFiles) {
  const fs::path box = kRoot / "box";
  const std::string compose = slurp(box / "docker-compose.yml");
  const std::string prometheus = slurp(box / "prometheus.yml");
  const std::string datasource = slurp(box / "datasource.yml");
  const std::string targets = slurp(box / "targets.yml");
  // The alert finds the job by name, and the dashboards the instrument by label.
  EXPECT_NE(prometheus.find("job_name: pychron"), std::string::npos);
  EXPECT_NE(prometheus.find("scrape_interval: 15s"), std::string::npos);
  EXPECT_NE(targets.find("instrument:"), std::string::npos);
  EXPECT_NE(targets.find(":9464"), std::string::npos);
  // The alert rules' data source is the one the box provisions.
  EXPECT_NE(compose.find("PYCHRON_PROM_UID: pychron-prometheus"), std::string::npos);
  EXPECT_NE(datasource.find("uid: pychron-prometheus"), std::string::npos);
  // Every file it mounts from the real box's set exists.
  for (const char* rel : {"grafana/provisioning/dashboards/pychron.yml", "grafana/provisioning/alerting/pychron-deadman.yml",
                          "grafana/dashboards"}) {
    EXPECT_NE(compose.find(std::string("../") + rel + ":"), std::string::npos) << rel;
    EXPECT_TRUE(fs::exists(kRoot / rel)) << rel;
  }
  // No sign-in, so this computer only.
  EXPECT_NE(compose.find("\"127.0.0.1:3000:3000\""), std::string::npos);
  EXPECT_NE(compose.find("\"127.0.0.1:9090:9090\""), std::string::npos);
  // The dashboards folder the provider reads is the one the compose file fills.
  const std::string provider = slurp(kRoot / "grafana" / "provisioning" / "dashboards" / "pychron.yml");
  EXPECT_NE(provider.find("path: /var/lib/grafana/dashboards/pychron"), std::string::npos);
  EXPECT_NE(compose.find(":/var/lib/grafana/dashboards/pychron:ro"), std::string::npos);
}

TEST(MetricsPackaging, TheScrapeJobIsNamedAsTheAlertExpects) {
  const std::string scrape = slurp(kRoot / "prometheus" / "pychron.scrape.yml");
  EXPECT_NE(scrape.find("job_name: pychron"), std::string::npos);
  EXPECT_NE(scrape.find(":9464"), std::string::npos);
  EXPECT_NE(scrape.find("instrument:"), std::string::npos);
}

TEST(MetricsPackaging, EveryQueryIsForOneInstrument) {
  // A dashboard for one instrument must not add up two.
  static const std::regex expr(R"lit("expr": "((?:[^"\\]|\\.)*)")lit");
  for (const fs::path& p : dashboards()) {
    const std::string text = slurp(p);
    int queries = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), expr), end; it != end; ++it) {
      ++queries;
      const std::string q = (*it)[1].str();
      EXPECT_NE(q.find("instrument=\\\"$instrument\\\""), std::string::npos) << p.filename() << ": " << q;
    }
    EXPECT_GT(queries, 0) << p.filename();
  }
}

#ifdef PYCHRON_TEST_HAS_JSON

TEST(MetricsPackaging, EveryDashboardIsValidJsonWithATitleAndUid) {
  std::set<std::string> uids;
  ASSERT_EQ(dashboards().size(), 3u);
  for (const fs::path& p : dashboards()) {
    const auto j = nlohmann::json::parse(slurp(p), nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << p.filename() << " is not JSON";
    EXPECT_FALSE(j.value("title", "").empty()) << p.filename();
    const std::string uid = j.value("uid", "");
    EXPECT_FALSE(uid.empty()) << p.filename();
    EXPECT_TRUE(uids.insert(uid).second) << "uid " << uid << " is used twice";
    EXPECT_FALSE(j.contains("id") && !j["id"].is_null()) << p.filename() << " carries the id of the Grafana it came from";
  }
}

TEST(MetricsPackaging, DashboardsTakeTheirDatasourceFromAVariable) {
  for (const fs::path& p : dashboards()) {
    const auto j = nlohmann::json::parse(slurp(p), nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    ASSERT_TRUE(j.contains("panels"));
    for (const auto& panel : j["panels"]) {
      if (panel.value("type", "") == "row") continue;
      ASSERT_TRUE(panel.contains("datasource")) << p.filename() << ": " << panel.value("title", "?");
      EXPECT_EQ(panel["datasource"].value("uid", ""), "${datasource}") << p.filename() << ": " << panel.value("title", "?");
    }
  }
}

TEST(MetricsPackaging, EveryDashboardHasItsVariables) {
  for (const fs::path& p : dashboards()) {
    const auto j = nlohmann::json::parse(slurp(p), nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    std::set<std::string> vars;
    for (const auto& v : j["templating"]["list"]) vars.insert(v.value("name", ""));
    EXPECT_EQ(vars.count("datasource"), 1u) << p.filename();
    EXPECT_EQ(vars.count("instrument"), 1u) << p.filename();
  }
}

TEST(MetricsPackaging, PanelsDoNotOverlapAndHaveDistinctIds) {
  for (const fs::path& p : dashboards()) {
    const auto j = nlohmann::json::parse(slurp(p), nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    std::set<int> ids;
    std::set<std::pair<int, int>> cells;
    for (const auto& panel : j["panels"]) {
      EXPECT_TRUE(ids.insert(panel.value("id", -1)).second) << p.filename() << ": " << panel.value("title", "?");
      const auto& g = panel["gridPos"];
      EXPECT_LE(g.value("x", 0) + g.value("w", 0), 24) << p.filename() << ": " << panel.value("title", "?");
      for (int x = g.value("x", 0); x < g.value("x", 0) + g.value("w", 0); ++x) {
        for (int y = g.value("y", 0); y < g.value("y", 0) + g.value("h", 0); ++y) {
          EXPECT_TRUE(cells.insert({x, y}).second)
              << p.filename() << ": " << panel.value("title", "?") << " overlaps at " << x << "," << y;
        }
      }
    }
  }
}

#endif  // PYCHRON_TEST_HAS_JSON

#endif  // PYCHRON_EXPERIMENT_HAS_METRICS
