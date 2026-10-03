#pragma once

// Composable reduction units (design section 7). A unit is a pure function
// with typed input and output ports and options described by a schema. A
// Pipeline is a DAG of unit instances; the Runner evaluates a node and caches
// every node's outputs under a fingerprint of its kind, options, inputs and
// (for units that read the source) the source generation, so changing one
// option recomputes only what depends on it.

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/group_results.hpp"
#include "pychron/processing/isotope_evolution_fit.hpp"
#include "pychron/processing/reference_fit.hpp"
#include "pychron/processing/scene.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

enum class PortType { Dataset, Scene, GroupResults, ReferenceFits, IsotopeFits };
std::string_view to_string(PortType t) noexcept;

using PortValue = std::variant<DatasetPtr, ScenePtr, GroupResultsPtr, ReferenceFitSetPtr, IsotopeFitSetPtr>;
PortType port_type(const PortValue& v) noexcept;

struct PortSpec {
  std::string name;
  PortType type = PortType::Dataset;
};

struct RunContext {
  IAnalysisSource* source = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  std::vector<std::string> diagnostics;  // warnings collected during the run
  bool cancelled() const noexcept { return cancel && cancel->load(); }
};

class Unit {
 public:
  virtual ~Unit() = default;
  virtual std::string_view kind() const = 0;
  virtual std::string_view title() const = 0;
  virtual const SchemaPtr& schema() const = 0;
  virtual std::vector<PortSpec> inputs() const = 0;
  virtual std::vector<PortSpec> outputs() const = 0;
  // Reads the analysis source (its results depend on the source generation).
  virtual bool reads_source() const { return false; }
  // Pure: no state kept between calls; `inputs` match inputs() in order and type.
  virtual Result<std::vector<PortValue>> execute(const std::vector<PortValue>& inputs, const Options& options,
                                                 RunContext& context) const = 0;
};

class UnitRegistry {
 public:
  void add(std::unique_ptr<Unit> unit);  // replaces a unit of the same kind
  const Unit* find(std::string_view kind) const;
  std::vector<std::string> kinds() const;
  // Every built-in unit (select, reduce, filter, group, edits, group_stats,
  // time_series, ideogram, spectrum, inverse_isochron, blank_fit,
  // icfactor_fit, isotope_evolution_fit).
  static const UnitRegistry& builtin();

 private:
  std::map<std::string, std::unique_ptr<Unit>, std::less<>> units_;
};

struct NodeSpec {
  std::string id;
  std::string kind;
  Options options;
  std::vector<std::string> inputs;  // "node" (output 0) or "node:port_index"
  std::string preset;               // informational: the preset the options came from
};

class Pipeline {
 public:
  std::string name;
  std::vector<NodeSpec> nodes;

  NodeSpec* find(std::string_view id);
  const NodeSpec* find(std::string_view id) const;
  // Adds a node with the unit's default options (or `options`).
  NodeSpec& add(const UnitRegistry& registry, std::string id, std::string kind, std::vector<std::string> inputs = {},
                std::optional<Options> options = std::nullopt);

  // Unknown kinds or ids, wrong input counts or types, options with another
  // unit's schema, and cycles; the message names the node.
  Result<void> validate(const UnitRegistry& registry) const;
  // Node ids in dependency order (validate() first).
  Result<std::vector<std::string>> order() const;
};

Result<Pipeline> pipeline_from_toml(const UnitRegistry& registry, std::string_view text,
                                    std::vector<std::string>* warnings = nullptr);
std::string pipeline_to_toml(const Pipeline& pipeline);

struct NodeRun {
  std::string id;
  bool executed = false;  // false: served from the cache
  std::optional<Error> error;
};

// Not thread-safe: one runner per worker thread. Cached values are immutable
// and may be read from any thread.
class Runner {
 public:
  Runner(const UnitRegistry& registry, IAnalysisSource* source) : registry_(registry), source_(source) {}

  // Evaluates `target` and what it depends on. A failing node's error is
  // returned for every node depending on it.
  Result<std::vector<PortValue>> run(const Pipeline& pipeline, std::string_view target,
                                     const std::atomic<bool>* cancel = nullptr);

  const std::vector<NodeRun>& last_run() const noexcept { return last_; }
  const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }
  std::size_t executions() const noexcept { return executions_; }
  void clear() { cache_.clear(); }

 private:
  struct Entry {
    std::string fingerprint;
    std::vector<PortValue> outputs;
  };

  const UnitRegistry& registry_;
  IAnalysisSource* source_;
  std::map<std::string, Entry> cache_;
  std::vector<NodeRun> last_;
  std::vector<std::string> diagnostics_;
  std::size_t executions_ = 0;
};

// Helpers for building datasets inside units.
DatasetPtr make_dataset(Dataset dataset);

}  // namespace pychron::processing
