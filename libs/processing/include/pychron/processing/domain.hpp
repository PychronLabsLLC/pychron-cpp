#pragma once

// A domain: what is specific to one kind of data reduction (domain seam spec
// 4). The core knows no isotope system; a domain says which units it adds to
// the pipeline, which figures the toolbar offers, what its derive pass caches
// for the browser, what the recall window shows beside the neutral rows, and
// what it can export. Qt-free; the UI half of a domain is `ui::DomainUi`.

#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/recall.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/source.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::processing {

struct FigureEntry {
  std::string kind;   // a unit kind in units()
  std::string label;  // toolbar / menu / palette text
  std::string icon;   // data_icon name; unknown name draws the generic figure glyph
  std::string tip;
  std::string group_key = "none";  // FigureWindow's default: "none" | "identifier" | "aliquot" | ...
  enum class Window { Figure, ReferenceFit, IsotopeEvolution } window = Window::Figure;
  bool separator_before = false;
};

// What the derive pass writes to derived_value and the browser may show.
struct DerivedQuantity {
  std::string name;   // derived_value.name ("age")
  std::string label;  // column header ("Age (Ma)")
  std::string units;
  bool filterable = true;  // browser's from/to filter
};

struct Exporter {
  std::string id, label, tip;
  std::string filters;  // file dialog filter ("CSV report (*.csv);;JSON report (*.json)")
  // Writes the analyses of `groups` to `path`. The exporter groups them as its
  // format wants (the Ar-Ar report by aliquot when any is a heating step, else
  // by identifier), whatever grouping they come in.
  std::function<Result<void>(const std::filesystem::path&, const std::vector<std::vector<ReducedPtr>>&)> write;
};

// Const methods are called from any thread: the bridge's worker calls
// `derive()` while the GUI thread calls `figures()`, `recall_sections()` and
// `exporters()`; a domain keeps no mutable state.
class Domain {
 public:
  virtual ~Domain() = default;
  virtual std::string_view id() const = 0;  // "noble_gas", "arar"
  virtual std::string_view title() const = 0;
  // Core builtins plus this domain's units; built once.
  virtual const UnitRegistry& units() const = 0;
  // Unit kinds after "select", in order, before filter/group.
  virtual std::vector<std::string> reduction_chain() const = 0;
  virtual std::vector<FigureEntry> figures() const = 0;     // toolbar order
  virtual std::vector<std::string> quantities() const = 0;  // names and stages Quantity may use beyond core's
  virtual std::string default_quantity() const = 0;
  // Browser column and filter; empty: no column, no filter, no cache join.
  // The first entry is the marker row: written for every derived analysis,
  // valueless when there is no value, and the name fingerprint lookups use.
  virtual std::vector<DerivedQuantity> derived() const = 0;
  // Nullopt when not cacheable (no input fingerprint); else one DerivedValues
  // with a value per derived().
  virtual std::optional<DerivedValues> derive(const ReducedAnalysis&) const = 0;
  // Part of every input fingerprint, and of a dataset's reduction tag.
  virtual std::string_view reduction_version() const = 0;
  // Recall window: sections after the neutral ones, and the error budget
  // (empty: no tab).
  virtual std::vector<RecallSection> recall_sections(const ReducedAnalysis&) const = 0;
  virtual std::vector<RecallErrorComponent> error_budget(const ReducedAnalysis&) const = 0;
  virtual std::vector<Exporter> exporters() const = 0;
};

// Filled by the app at startup: apps link the domains, processing never does.
class DomainRegistry {
 public:
  void add(std::unique_ptr<Domain> domain);  // replaces one of the same id
  const Domain* find(std::string_view id) const;
  std::vector<std::string> ids() const;  // sorted

 private:
  std::map<std::string, std::unique_ptr<Domain>, std::less<>> domains_;
};

std::unique_ptr<Domain> make_noble_gas_domain();  // core only

// The Ar-Ar domain over today's code. Moves to libs/arar (`pychron/arar/domain.hpp`)
// when the domain seam splits that library out.
std::unique_ptr<Domain> make_arar_domain();

}  // namespace pychron::processing
