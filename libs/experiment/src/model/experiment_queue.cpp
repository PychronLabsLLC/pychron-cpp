#include "pychron/experiment/model/experiment_queue.hpp"

#include <algorithm>
#include <random>
#include <tuple>

namespace pychron::experiment {
namespace {

Unexpected<Error> bad(std::string what) { return fail(ErrorKind::Config, std::move(what)); }

// Uniform in [0, n) without std::uniform_int_distribution, whose algorithm is
// implementation-defined; the same seed must give the same order everywhere.
std::size_t uniform_below(std::mt19937_64& rng, std::size_t n) {
  const std::uint64_t range = n;
  const std::uint64_t limit = std::mt19937_64::max() - std::mt19937_64::max() % range;
  std::uint64_t x = 0;
  do x = rng();
  while (x >= limit);
  return static_cast<std::size_t>(x % range);
}

}  // namespace

Result<std::vector<std::size_t>> ExperimentQueue::check_rows(std::vector<std::size_t> rows) const {
  if (rows.empty()) return bad("no rows selected");
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  if (rows.back() >= size()) return bad("row " + std::to_string(rows.back()) + " out of range");
  return rows;
}

void ExperimentQueue::append(RunSpec run) { spec_.runs.push_back(std::move(run)); }

Result<void> ExperimentQueue::insert(std::size_t at, RunSpec run) {
  if (at > size()) return bad("insert position " + std::to_string(at) + " out of range");
  spec_.runs.insert(spec_.runs.begin() + static_cast<std::ptrdiff_t>(at), std::move(run));
  return {};
}

Result<void> ExperimentQueue::remove(std::vector<std::size_t> rows) {
  auto sel = check_rows(std::move(rows));
  if (!sel) return fail(sel.error());
  for (auto it = sel->rbegin(); it != sel->rend(); ++it)
    spec_.runs.erase(spec_.runs.begin() + static_cast<std::ptrdiff_t>(*it));
  return {};
}

Result<void> ExperimentQueue::replace(std::size_t row, RunSpec run) {
  if (row >= size()) return bad("row " + std::to_string(row) + " out of range");
  spec_.runs[row] = std::move(run);
  return {};
}

Result<void> ExperimentQueue::replace_from(std::size_t from, std::vector<RunSpec> runs) {
  if (from > size()) return bad("row " + std::to_string(from) + " out of range");
  spec_.runs.resize(from);
  for (auto& r : runs) spec_.runs.push_back(std::move(r));
  return {};
}

Result<void> ExperimentQueue::move(std::vector<std::size_t> rows, std::size_t to) {
  if (to > size()) return bad("move target " + std::to_string(to) + " out of range");
  auto sel = check_rows(std::move(rows));
  if (!sel) return fail(sel.error());
  std::vector<RunSpec> moved, rest;
  std::size_t at = 0;  // insertion point within `rest`
  for (std::size_t i = 0, k = 0; i < size(); ++i) {
    if (k < sel->size() && (*sel)[k] == i) {
      moved.push_back(std::move(spec_.runs[i]));
      ++k;
    } else {
      if (i < to) ++at;
      rest.push_back(std::move(spec_.runs[i]));
    }
  }
  rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(moved.begin()),
              std::make_move_iterator(moved.end()));
  spec_.runs = std::move(rest);
  return {};
}

Result<void> ExperimentQueue::copy(std::vector<std::size_t> rows, std::size_t to) {
  if (to > size()) return bad("copy target " + std::to_string(to) + " out of range");
  auto sel = check_rows(std::move(rows));
  if (!sel) return fail(sel.error());
  std::vector<RunSpec> copies;
  for (std::size_t i : *sel) copies.push_back(spec_.runs[i]);
  spec_.runs.insert(spec_.runs.begin() + static_cast<std::ptrdiff_t>(to), copies.begin(), copies.end());
  return {};
}

Result<void> ExperimentQueue::repeat_block(std::size_t first, std::size_t count, std::size_t times) {
  if (count == 0 || times == 0) return bad("repeat_block needs a non-empty block and times >= 1");
  if (first >= size() || count > size() - first) return bad("repeat block out of range");
  const auto begin = spec_.runs.begin() + static_cast<std::ptrdiff_t>(first);
  std::vector<RunSpec> block(begin, begin + static_cast<std::ptrdiff_t>(count));
  std::vector<RunSpec> repeated;
  repeated.reserve(count * times);
  for (std::size_t t = 0; t < times; ++t) repeated.insert(repeated.end(), block.begin(), block.end());
  spec_.runs.insert(spec_.runs.begin() + static_cast<std::ptrdiff_t>(first + count), repeated.begin(), repeated.end());
  return {};
}

Result<void> ExperimentQueue::randomize(std::uint64_t seed, std::vector<std::size_t> rows) {
  if (rows.empty()) {
    rows.resize(size());
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = i;
    if (rows.empty()) return {};
  }
  auto sel = check_rows(std::move(rows));
  if (!sel) return fail(sel.error());
  std::vector<RunSpec> picked;
  for (std::size_t i : *sel) picked.push_back(std::move(spec_.runs[i]));
  std::mt19937_64 rng(seed);
  for (std::size_t i = picked.size(); i > 1; --i) std::swap(picked[i - 1], picked[uniform_below(rng, i)]);
  for (std::size_t k = 0; k < sel->size(); ++k) spec_.runs[(*sel)[k]] = std::move(picked[k]);
  return {};
}

void ExperimentQueue::group_by_extraction() {
  auto same = [](const RunSpec& a, const RunSpec& b) {
    const auto& x = a.extraction;
    const auto& y = b.extraction;
    return std::tie(x.device, x.value, x.units) == std::tie(y.device, y.value, y.units);
  };
  std::vector<std::vector<RunSpec>> groups;
  for (auto& r : spec_.runs) {
    auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& grp) { return same(grp.front(), r); });
    if (g == groups.end()) groups.push_back({std::move(r)});
    else g->push_back(std::move(r));
  }
  spec_.runs.clear();
  for (auto& g : groups)
    for (auto& r : g) spec_.runs.push_back(std::move(r));
}

Result<void> ExperimentQueue::toggle_skip(const std::vector<std::size_t>& rows) {
  auto sel = check_rows(rows);
  if (!sel) return fail(sel.error());
  for (std::size_t i : *sel) spec_.runs[i].skip = !spec_.runs[i].skip;
  return {};
}

Result<void> ExperimentQueue::toggle_end_after(std::size_t row) {
  if (row >= size()) return bad("row " + std::to_string(row) + " out of range");
  const bool set = !spec_.runs[row].end_after;
  for (auto& r : spec_.runs) r.end_after = false;
  spec_.runs[row].end_after = set;
  return {};
}

Result<std::size_t> ExperimentQueue::add_frequency_runs(const FrequencySpec& f) {
  if (f.every < 0) return bad("frequency must be >= 0");
  if (f.every == 0 && !f.before && !f.after) return bad("frequency run needs every > 0, before or after");
  const std::size_t last = f.last.value_or(size());
  if (last > size() || f.first > last) return bad("frequency range out of range");

  std::vector<std::size_t> counted;
  for (std::size_t i = f.first; i < last; ++i) {
    const auto& r = spec_.runs[i];
    if (!r.skip && f.counted.count(r.id.type)) counted.push_back(i);
  }
  if (counted.empty()) return std::size_t{0};

  // Insert-before positions in original row indexing.
  std::set<std::size_t> slots;
  if (f.before) slots.insert(counted.front());
  if (f.every > 0)
    for (std::size_t j = 0; j < counted.size(); ++j)
      if ((j + 1) % static_cast<std::size_t>(f.every) == 0) slots.insert(counted[j] + 1);
  if (f.after) slots.insert(counted.back() + 1);

  for (auto it = slots.rbegin(); it != slots.rend(); ++it)
    spec_.runs.insert(spec_.runs.begin() + static_cast<std::ptrdiff_t>(*it), f.run);
  return slots.size();
}

}  // namespace pychron::experiment
