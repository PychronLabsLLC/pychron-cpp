// The arar_reduce unit (domain seam spec 3.2): after core's reduce, the Ar-Ar
// reduction of every item, and the derived values it makes in the source's
// cache.

#include <string>
#include <utility>
#include <vector>

#include "pychron/arar/reduce.hpp"
#include "pychron/processing/domain.hpp"
#include "schema_builder.hpp"

namespace pychron::arar {

namespace {

namespace pp = pychron::processing;

class ArArReduceUnit final : public pp::Unit {
 public:
  ArArReduceUnit()
      : schema_(pp::detail::make_schema(
            "unit.arar_reduce", "Ar-Ar reduction",
            {pp::detail::choice("constants", "Constants", "Reduction", {"default", "legacy", "legacy_preferences"}),
             pp::detail::boolean("include_decay_error", "Include decay constant error", "Reduction", false),
             pp::detail::boolean("use_irradiation_endtime", "Decay from the irradiation end", "Reduction", false)})) {}

  std::string_view kind() const override { return "arar_reduce"; }
  std::string_view title() const override { return "Ar-Ar reduction"; }
  const pp::SchemaPtr& schema() const override { return schema_; }
  std::vector<pp::PortSpec> inputs() const override { return {{"reduced", pp::PortType::Dataset}}; }
  std::vector<pp::PortSpec> outputs() const override { return {{"reduced", pp::PortType::Dataset}}; }

  Result<std::vector<pp::PortValue>> execute(const std::vector<pp::PortValue>& in, const pp::Options& o,
                                             pp::RunContext& ctx) const override {
    ArArSettings s;
    const std::string c = o.get_string("constants");
    s.preset = c == "legacy"               ? reduction::ConstantsPreset::Legacy
               : c == "legacy_preferences" ? reduction::ConstantsPreset::LegacyPreferences
                                           : reduction::ConstantsPreset::Default;
    s.include_decay_error = o.get_bool("include_decay_error");
    s.use_irradiation_endtime = o.get_bool("use_irradiation_endtime");
    pp::Dataset d = *std::get<pp::DatasetPtr>(in.at(0));
    std::size_t failed = 0;
    for (auto& item : d.mutable_items()) {
      if (ctx.cancelled()) return fail(ErrorKind::Cancelled, "arar_reduce: cancelled");
      auto out = std::make_shared<pp::ReducedAnalysis>(*item.analysis);
      out->domain.reset();
      if (auto reduced = reduce_arar(*item.analysis, s)) {
        out->reduction_error = (*reduced)->reduction_error;
        out->domain = std::move(*reduced);
      } else {
        out->reduction_error = reduced.error().what;
      }
      if (!out->reduction_error.empty()) ++failed;
      item.analysis = std::move(out);
    }
    if (failed) ctx.diagnostics.push_back(std::to_string(failed) + " analyses did not reduce");
    // Every reduction the application makes keeps its derived values in the
    // source's cache, when it has one (age cache spec 4.5); the sink drops the
    // current. The analyses here carry their stored fits: one with unsaved fit
    // edits keeps the stored input fingerprint and must never reach this unit.
    // The cache is the domain's to derive; a run without one writes nothing. A
    // reduction made under other than the default settings is no cached one.
    if (pp::IDerivedSink* sink = ctx.source && ctx.domain && s == ArArSettings{} ? ctx.source->derived() : nullptr) {
      std::vector<pp::DerivedValues> values;
      for (const auto& item : d.items())
        if (auto derived = ctx.domain->derive(*item.analysis)) values.push_back(std::move(*derived));
      if (!values.empty()) sink->put(std::move(values));
    }
    const std::string_view version = ctx.domain ? ctx.domain->reduction_version() : reduction::kReductionVersion;
    d.reduction_tag = std::string(version) + "/" + c + "/" + (s.include_decay_error ? "d1" : "d0") +
                      (s.use_irradiation_endtime ? "e1" : "e0");
    return std::vector<pp::PortValue>{pp::PortValue(pp::make_dataset(std::move(d)))};
  }

 private:
  pp::SchemaPtr schema_;
};

}  // namespace

std::unique_ptr<processing::Unit> make_arar_reduce_unit() { return std::make_unique<ArArReduceUnit>(); }

}  // namespace pychron::arar
