// The selection, reduction and plateau flags of `elctl export` and `elctl
// earthbank`, and the load of a selection from the store.

#include "selection.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "pychron/core/number.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/arar/arar_types.hpp"

namespace elctl {

namespace fs = std::filesystem;
namespace pp = pychron::processing;
namespace ps = pychron::persistence;
namespace r = pychron::reduction;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

namespace {

// "YYYY-MM-DD" as UTC epoch seconds of its midnight.
std::optional<double> parse_date(std::string_view text) {
  const auto first = text.find('-', 1);  // past a year's own sign
  const auto second = first == std::string_view::npos ? first : text.find('-', first + 1);
  if (second == std::string_view::npos) return std::nullopt;
  const auto y = parse_int(text.substr(0, first));
  const auto m = parse_int(text.substr(first + 1, second - first - 1));
  const auto d = parse_int(text.substr(second + 1));
  if (!y || !m || !d || *m < 1 || *d < 1) return std::nullopt;
  using namespace std::chrono;
  const year_month_day ymd{year{*y}, month{static_cast<unsigned>(*m)}, day{static_cast<unsigned>(*d)}};
  if (!ymd.ok()) return std::nullopt;
  return static_cast<double>(sys_days{ymd}.time_since_epoch().count()) * 86400.0;
}

// The value of the flag at `args[i]`, which every flag but a switch has.
Result<const std::string*> value_of(const std::vector<std::string>& args, std::size_t& i) {
  if (i + 1 >= args.size()) return fail(ErrorKind::Config, args[i] + " needs a value");
  return &args[++i];
}

}  // namespace

// All of `text` as a whole number, or nothing: a number that does not fit is
// not a number here (sscanf's %d wraps one into range, or worse).
std::optional<int> parse_int(std::string_view text) {
  int value = 0;
  const char* end = text.data() + text.size();
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): from_chars is given the end
  const auto [stop, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || stop != end) return std::nullopt;
  return value;
}

pychron::arar::ArArSettings AgeFlags::reduction() const {
  pychron::arar::ArArSettings s;
  for (const auto p : {r::ConstantsPreset::Default, r::ConstantsPreset::Legacy, r::ConstantsPreset::LegacyPreferences})
    if (r::to_string(p) == constants) s.preset = p;
  s.include_decay_error = decay_error;
  return s;
}

r::PlateauCriteria AgeFlags::criteria() const {
  r::PlateauCriteria c;
  c.method = plateau == "mahon" ? r::PlateauMethod::Mahon : r::PlateauMethod::Fleck;
  c.nsteps = plateau_steps;
  c.gas_fraction = plateau_gas;
  return c;
}

Result<bool> take_selection_flag(Selection& s, const std::vector<std::string>& args, std::size_t& i) {
  const std::string& flag = args[i];
  if (flag == "--include-invalid") {
    s.include_invalid = true;
    return true;
  }
  std::vector<std::string>* list = flag == "--sample"        ? &s.samples
                                   : flag == "--identifier"  ? &s.identifiers
                                   : flag == "--project"     ? &s.projects
                                   : flag == "--irradiation" ? &s.irradiations
                                   : flag == "--type"        ? &s.types
                                   : flag == "--uuid"        ? &s.uuids
                                                             : nullptr;
  if (!list && flag != "--from" && flag != "--to" && flag != "--limit") return false;
  const auto value = value_of(args, i);
  if (!value) return fail(value.error());
  if (list) {
    list->push_back(**value);
  } else if (flag == "--limit") {
    const auto n = parse_int(**value);
    if (!n || *n < 1) return fail(ErrorKind::Config, "--limit takes a whole positive number");
    s.limit = *n;
  } else {
    const auto t = parse_date(**value);
    if (!t) return fail(ErrorKind::Config, flag + " takes a UTC date YYYY-MM-DD; got '" + **value + "'");
    (flag == "--from" ? s.from : s.to) = flag == "--from" ? *t : *t + 86399.0;  // --to: the whole day
  }
  return true;
}

Result<bool> take_age_flag(AgeFlags& a, const std::vector<std::string>& args, std::size_t& i) {
  const std::string& flag = args[i];
  if (flag == "--decay-error") {
    a.decay_error = true;
    return true;
  }
  if (flag != "--constants" && flag != "--plateau" && flag != "--plateau-steps" && flag != "--plateau-gas") return false;
  const auto found = value_of(args, i);
  if (!found) return fail(found.error());
  const std::string& value = **found;
  if (flag == "--constants") {
    bool known = false;
    for (const auto p : {r::ConstantsPreset::Default, r::ConstantsPreset::Legacy, r::ConstantsPreset::LegacyPreferences})
      known = known || r::to_string(p) == value;
    if (!known) return fail(ErrorKind::Config, "--constants is default, legacy or legacy_preferences; got '" + value + "'");
    a.constants = value;
  } else if (flag == "--plateau") {
    if (value != "fleck" && value != "mahon") return fail(ErrorKind::Config, "--plateau is fleck or mahon");
    a.plateau = value;
  } else if (flag == "--plateau-steps") {
    const auto n = parse_int(value);
    if (!n || *n < 2) return fail(ErrorKind::Config, "--plateau-steps takes a whole number of 2 or more");
    a.plateau_steps = *n;
  } else {
    const auto pct = pychron::parse_double(value);
    if (!pct || !(*pct >= 0 && *pct <= 100)) return fail(ErrorKind::Config, "--plateau-gas takes a percent, 0 to 100");
    a.plateau_gas = *pct;
  }
  return true;
}

Result<void> check_selection(const Selection& s) {
  if (s.from && s.to && *s.to < *s.from) return fail(ErrorKind::Config, "--to is before --from");
  return {};
}

Result<std::unique_ptr<pp::StoreSource>> open_source(const std::string& db_url) {
  // A mistyped SQLite path is an error, not a new empty store.
  constexpr std::string_view kSqlite = "sqlite:";
  if (db_url.starts_with(kSqlite) && db_url != "sqlite::memory:") {
    std::error_code code;
    const fs::path file(db_url.substr(kSqlite.size()));
    if (!fs::is_regular_file(file, code)) return fail(ErrorKind::Io, "no database at " + file.string());
    if (fs::file_size(file, code) == 0 && !code) return fail(ErrorKind::Io, file.string() + " is empty: not a pychron store");
  }
  return pp::StoreSource::open(ps::StoreConfig{db_url, false}, pp::StoreSourceOptions{1, "", ""});
}

Result<void> add_select(pp::Pipeline& pipeline, const pp::UnitRegistry& registry, const Selection& s) {
  auto& select = pipeline.add(registry, "select", "select");
  std::optional<pychron::Error> bad;
  auto set = [&](std::string_view key, pp::OptionValue value) {
    auto ok = select.options.set(key, std::move(value));
    if (!ok && !bad) bad = ok.error();
  };
  set("uuids", s.uuids);
  set("samples", s.samples);
  set("identifiers", s.identifiers);
  set("projects", s.projects);
  set("irradiations", s.irradiations);
  set("analysis_types", s.types.empty() ? std::vector<std::string>{"unknown"} : s.types);
  if (s.from) set("from", *s.from);
  if (s.to) set("to", *s.to);
  set("limit", static_cast<std::int64_t>(s.limit));
  set("remove_tags", s.include_invalid ? std::vector<std::string>{} : std::vector<std::string>{"invalid"});
  if (bad) return fail(*bad);
  return {};
}

Result<std::vector<pp::AnalysisPtr>> load_selection(const Selection& selection, const std::string& db_url,
                                                    std::vector<std::string>& warnings) {
  auto source = open_source(db_url);
  if (!source) return fail(source.error());
  const auto& registry = pp::UnitRegistry::builtin();
  pp::Pipeline pipeline;
  pipeline.name = "selection";
  if (auto added = add_select(pipeline, registry, selection); !added) return fail(added.error());
  pp::Runner runner(registry, source->get());
  auto selected = runner.run(pipeline, "select");
  if (!selected) return fail(selected.error());
  for (const auto& d : runner.diagnostics()) warnings.push_back(d);
  std::vector<pp::AnalysisPtr> analyses;
  for (const auto& item : std::get<pp::DatasetPtr>(selected->at(0))->items()) analyses.push_back(item.analysis->analysis);
  return analyses;
}

}  // namespace elctl
