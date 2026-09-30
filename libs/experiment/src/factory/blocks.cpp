#include "pychron/experiment/factory/blocks.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/experiment/model/queue_toml.hpp"
#include "pychron/experiment/model/rules.hpp"

namespace pychron::experiment {

Result<Block> parse_block(std::string_view text, const IdentifierRules& ids, std::string_view name) {
  auto parsed = toml::parse(text, name);
  if (!parsed) return fail(ErrorKind::Config, std::string(name) + ": syntax error: " + std::string(parsed.error().description()));
  const toml::table& root = parsed.table();
  const std::string file(name);

  for (const auto& [k, v] : root)
    if (k.str() != "block" && k.str() != "runs")
      return fail(ErrorKind::Config, file + ": root: unknown key '" + std::string(k.str()) + "'");
  const auto* bt = root["block"].as_table();
  if (!bt) return fail(ErrorKind::Config, file + ": missing [block] table");

  Block block;
  for (const auto& [k, v] : *bt) {
    auto s = v.value<std::string>();
    if (k.str() == "name" && s) block.name = *s;
    else if (k.str() == "description" && s) block.description = *s;
    else return fail(ErrorKind::Config, file + ": block: bad key '" + std::string(k.str()) + "'");
  }
  if (block.name.empty()) return fail(ErrorKind::Config, file + ": block.name is required");
  const auto* runs = root["runs"].as_array();
  if (!runs || runs->empty()) return fail(ErrorKind::Config, file + ": block has no [[runs]]");

  // Runs share the queue schema: reuse the queue parser (aliases, key checks, type derivation).
  toml::table as_queue;
  as_queue.insert("queue", toml::table{});
  as_queue.insert("runs", *runs);
  std::ostringstream ss;
  ss << as_queue;
  auto q = parse_queue(ss.str(), ids, name);
  if (!q) return fail(q.error());
  block.runs = std::move(q->runs);
  return block;
}

Result<Block> load_block(const std::string& path, const IdentifierRules& ids) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_block(ss.str(), ids, path);
}

std::vector<RunSpec> instantiate_block(const Block& block, const BlockContext& ctx) {
  std::vector<RunSpec> out = block.runs;
  for (auto& run : out) {
    const FieldRules f = rules_for(run.id.type);
    if (run.extraction.device.empty() && f.extraction) run.extraction.device = ctx.extract_device;
    if (ctx.defaults && f.measurement && run.measurement.plan.empty() && !run.measurement.hook) {
      if (const RunDefaults* d = ctx.defaults->find(run.id.type, run.extraction.device)) apply_defaults(run, *d);
    }
    strip_for_type(run);
  }
  return out;
}

std::vector<RunSpec> insert_runs(const std::vector<RunSpec>& runs, std::size_t index, const std::vector<RunSpec>& insert) {
  std::vector<RunSpec> out;
  out.reserve(runs.size() + insert.size());
  const auto at = runs.begin() + static_cast<std::ptrdiff_t>(std::min(index, runs.size()));
  out.insert(out.end(), runs.begin(), at);
  out.insert(out.end(), insert.begin(), insert.end());
  out.insert(out.end(), at, runs.end());
  return out;
}

std::vector<RunSpec> repeat_block(const std::vector<RunSpec>& runs, int times) {
  std::vector<RunSpec> out;
  for (int i = 0; i < times; ++i) out.insert(out.end(), runs.begin(), runs.end());
  return out;
}

}  // namespace pychron::experiment
