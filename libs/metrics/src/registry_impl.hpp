#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pychron/metrics/registry.hpp"

namespace pychron::metrics {

struct CollectorHandle::Collectors {
  // Held while a collector runs, so reset() returns only when it is not.
  std::recursive_mutex mutex;
  std::map<std::uint64_t, Registry::Collector> items;
  std::uint64_t next_id = 1;
};

namespace detail {

struct Series {
  std::unique_ptr<Counter> counter;
  std::unique_ptr<Gauge> gauge;
  std::unique_ptr<Histogram> histogram;
  bool hidden = false;
};

struct Family {
  MetricType type = MetricType::Gauge;
  std::string help;
  std::vector<double> buckets;
  std::map<Labels, Series> series;  // labels sorted by name
  bool reported_full = false;
};

std::string render_text(const std::map<std::string, Family, std::less<>>& families);

}  // namespace detail

}  // namespace pychron::metrics
