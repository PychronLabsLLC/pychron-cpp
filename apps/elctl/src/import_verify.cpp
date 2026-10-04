// elctl import verify: run ingest::verify over each source and print what it
// found (legacy ingestion spec, sections 6 and 10.26 to 10.32).

#include <cmath>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "import_impl.hpp"

namespace elctl::import_detail {

namespace ingest = pychron::ingest;

namespace {

using Json = nlohmann::json;

using pychron::reduction::ConstantsPreset;
constexpr ConstantsPreset kConstantsPresets[] = {ConstantsPreset::Default, ConstantsPreset::Legacy,
                                                 ConstantsPreset::LegacyPreferences};
// The preset that reproduces the ages legacy pychron stored: its preference
// defaults (atmospheric 40Ar/36Ar 295.5). Measured on the IR1010 fixture:
// age to 5e-9, error to 5e-9; `default` (298.56) is 2.4e-3 off.
constexpr ConstantsPreset kDefaultConstants = ConstantsPreset::LegacyPreferences;

// Unaccounted units printed before "and N more" (--json lists them all).
constexpr std::size_t kListed = 20;

const char* name_of(ingest::UnitDisposition disposition) {
  switch (disposition) {
    case ingest::UnitDisposition::Ignored:
      return "ignored";
    case ingest::UnitDisposition::Imported:
      return "imported";
    case ingest::UnitDisposition::Folded:
      return "folded";
    case ingest::UnitDisposition::Unchanged:
      return "unchanged";
    case ingest::UnitDisposition::Conflict:
      return "conflict";
    case ingest::UnitDisposition::Removed:
      return "removed";
    case ingest::UnitDisposition::Unclassified:
      return "unclassified";
  }
  return "unclassified";
}

const char* name_of(ingest::Evidence::Kind kind) {
  switch (kind) {
    case ingest::Evidence::Kind::Revision:
      return "revision";
    case ingest::Evidence::Kind::Analysis:
      return "analysis";
    case ingest::Evidence::Kind::Conflict:
      return "conflict";
    case ingest::Evidence::Kind::Entity:
      return "entity";
    case ingest::Evidence::Kind::Note:
      return "note";
    case ingest::Evidence::Kind::CatalogRow:
      return "catalog_row";
  }
  return "revision";
}

// "<path> @<commit>" and what else names the row: the analysis, the list.
std::string describe(const ingest::Evidence& e) {
  std::string out = std::string(name_of(e.kind)) + " " + e.path + " @" + e.commit;
  if (e.kind == ingest::Evidence::Kind::Analysis || e.kind == ingest::Evidence::Kind::Entity) out += " " + e.entity.str();
  if (e.kind == ingest::Evidence::Kind::Note) out += " in " + e.list;
  return out;
}

std::string number(double value) {
  std::ostringstream text;
  text.precision(12);
  text << value;
  return text.str();
}

// A relative residual: two significant digits, as in 4.9e-09.
std::string residual(double value) {
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << std::scientific << std::setprecision(1) << value;
  return text.str();
}

std::string plural(int n, const char* noun) { return std::to_string(n) + " " + noun + (n == 1 ? "" : "s"); }

// Why the report is not ok; empty: it is.
std::vector<std::string> reasons(const ingest::VerifyReport& r) {
  std::vector<std::string> out;
  if (!r.source.registered) {
    out.push_back("this source was never imported");
  } else if (r.source.status != "finished") {
    out.push_back("the import has not finished; run it");
  } else if (!r.source.finished_and_current()) {
    out.push_back("the source has changed since the last import; run the import, then verify");
  }
  if (!r.unaccounted.empty()) out.push_back(std::to_string(r.unaccounted.size()) + " unaccounted");
  if (r.would_write != 0 || r.replay_would_write != 0)
    out.push_back(std::to_string(r.would_write) + " rows to write on resume, " + std::to_string(r.replay_would_write) +
                  " on replay");
  if (r.pending_blocking != 0) out.push_back(plural(r.pending_blocking, "blocking conflict"));
  if (r.parity_fail != 0) out.push_back(plural(r.parity_fail, "parity failure"));
  return out;
}

void print(std::ostream& out, const Source& source, const ingest::VerifyReport& r, ConstantsPreset constants) {
  out << source.name << " (" << P::to_string(source.info.spec.kind) << ") " << source.info.spec.uuid.str() << '\n';

  out << "  import: ";
  if (!r.source.registered) {
    out << "not registered";
  } else {
    out << r.source.status << ' ' << r.source.done << '/' << r.source.total << ", head "
        << r.source.stored_head.value_or("none");
    if (!r.source.stored_head || *r.source.stored_head != r.source.current_head)
      out << ", the source is now at " << r.source.current_head;
  }
  out << '\n';

  out << "  accounting: " << r.units << " units, " << r.ignored << " ignored, " << r.unaccounted.size()
      << " unaccounted\n";
  for (std::size_t i = 0; i < r.unaccounted.size() && i < kListed; ++i) {
    const auto& open = r.unaccounted[i];
    out << "    " << name_of(open.unit.disposition) << ' ' << open.unit.path << " @" << open.unit.commit;
    if (!open.unit.repeats.empty()) out << " repeats " << open.unit.repeats;
    out << '\n';
    if (open.missing.empty()) out << "      names no row\n";
    for (const auto& evidence : open.missing) out << "      missing " << describe(evidence) << '\n';
  }
  if (r.unaccounted.size() > kListed) out << "    and " << r.unaccounted.size() - kListed << " more\n";

  out << "  idempotence: " << r.would_write << " rows to write on resume, " << r.replay_would_write << " on replay\n";

  out << "  parity: " << r.parity_pass << " pass, " << r.parity_pass_age_only << " pass on age only, " << r.parity_fail
      << " fail, " << r.parity_not_comparable
      << " not comparable (constants " << pychron::reduction::to_string(constants)
      << "; members of each interpreted age's head revision, as of the commit that saved it)\n";
  for (const auto& [reason, members] : r.not_comparable_reasons)
    out << "    not comparable: " << reason << ' ' << members << '\n';
  for (const auto& f : r.parity_failures) {
    out << "    fail " << f.analysis.str() << " in " << f.interpreted_age.str() << ": legacy " << number(f.legacy_age);
    if (f.legacy_age_err) out << " +- " << number(*f.legacy_age_err);
    out << ", computed " << number(f.computed_age);
    if (f.legacy_age_err) out << " +- " << number(f.computed_age_err) << " (" << f.error_compared << ')';
    out << '\n';
  }
  if (r.parity_pass + r.parity_pass_age_only > 0) {
    out << "    largest passing residual: age " << residual(r.parity_max_pass_age_difference);
    if (r.parity_pass > 0) out << ", error " << residual(r.parity_max_pass_age_err_difference);
    out << '\n';
  }

  out << "  conflicts pending: " << r.pending_blocking << " blocking, " << plural(r.pending_warnings, "warning") << '\n';

  const auto why = reasons(r);
  if (why.empty()) {
    out << "  ok\n";
    return;
  }
  out << "  not ok: ";
  for (std::size_t i = 0; i < why.size(); ++i) out << (i ? "; " : "") << why[i];
  out << '\n';
}

Json to_json(const Source& source, const ingest::VerifyReport& r, ConstantsPreset constants) {
  Json unaccounted = Json::array();
  for (const auto& open : r.unaccounted) {
    Json missing = Json::array();
    for (const auto& e : open.missing) {
      Json row{{"kind", name_of(e.kind)}, {"commit", e.commit}, {"path", e.path}};
      if (e.kind == ingest::Evidence::Kind::Analysis || e.kind == ingest::Evidence::Kind::Entity)
        row["entity"] = e.entity.str();
      if (e.kind == ingest::Evidence::Kind::Note) row["list"] = e.list;
      missing.push_back(std::move(row));
    }
    Json unit{{"commit", open.unit.commit},
              {"path", open.unit.path},
              {"disposition", name_of(open.unit.disposition)},
              {"missing", std::move(missing)}};
    if (!open.unit.repeats.empty()) unit["repeats"] = open.unit.repeats;
    unaccounted.push_back(std::move(unit));
  }
  Json failures = Json::array();
  for (const auto& f : r.parity_failures) {
    Json row{{"analysis", f.analysis.str()},
             {"interpreted_age", f.interpreted_age.str()},
             {"conflict", f.conflict.str()},
             {"legacy_age", f.legacy_age},
             {"computed_age", f.computed_age},
             {"computed_age_err", f.computed_age_err},
             {"age_difference", f.age_difference},
             {"age_err_difference", f.age_err_difference}};
    if (f.legacy_age_err) {
      row["legacy_age_err"] = *f.legacy_age_err;
      row["error_compared"] = f.error_compared;
    }
    if (!f.basis.empty()) row["basis"] = f.basis;
    failures.push_back(std::move(row));
  }
  const auto ids = [](const std::vector<P::Uuid>& uuids) {
    Json out = Json::array();
    for (const auto& uuid : uuids) out.push_back(uuid.str());
    return out;
  };
  return Json{
      {"uuid", source.info.spec.uuid.str()},
      {"kind", std::string(P::to_string(source.info.spec.kind))},
      {"name", source.name},
      {"url", source.info.spec.url_or_path},
      {"import",
       {{"registered", r.source.registered},
        {"status", r.source.status},
        {"done", r.source.done},
        {"total", r.source.total},
        {"stored_head", r.source.stored_head ? Json(*r.source.stored_head) : Json(nullptr)},
        {"current_head", r.source.current_head}}},
      {"accounting", {{"units", r.units}, {"ignored", r.ignored}, {"unaccounted", std::move(unaccounted)}}},
      {"idempotence", {{"would_write", r.would_write}, {"replay_would_write", r.replay_would_write}}},
      {"parity",
       {{"constants", std::string(pychron::reduction::to_string(constants))},
        {"pass", r.parity_pass},
        {"pass_age_only", r.parity_pass_age_only},
        {"fail", r.parity_fail},
        {"max_pass_age_difference", r.parity_max_pass_age_difference},
        {"max_pass_age_err_difference", r.parity_max_pass_age_err_difference},
        {"not_comparable_total", r.parity_not_comparable},
        {"not_comparable", Json(r.not_comparable_reasons)},
        {"failures", std::move(failures)}}},
      {"conflicts",
       {{"blocking", ids(r.blocking_conflicts)}, {"warnings", ids(r.warning_conflicts)}}},
      {"ok", r.ok()},
      {"reasons", reasons(r)}};
}

}  // namespace

int import_verify(Context& ctx, const Flags& flags) {
  ingest::VerifyOptions options;
  if (const auto text = flags.get("--tolerance")) {
    // A stream in the C locale: floating-point from_chars is missing from older libc++.
    std::istringstream in(*text);
    in.imbue(std::locale::classic());
    double value = 0;
    char rest = 0;
    if (!(in >> value) || in.get(rest) || !std::isfinite(value) || value < 0)
      return fatal(ctx.io, "--tolerance takes a relative difference, 0 or more; got '" + *text + "'");
    options.tolerance = value;
  }
  auto constants = kDefaultConstants;
  if (const auto text = flags.get("--constants")) {
    bool known = false;
    for (const auto preset : kConstantsPresets)
      if (pychron::reduction::to_string(preset) == *text) {
        constants = preset;
        known = true;
      }
    if (!known) return fatal(ctx.io, "--constants is default, legacy or legacy_preferences; got '" + *text + "'");
  }
  auto all = registered_sources(ctx);
  if (!all) return fatal(ctx.io, all.error());
  auto chosen = select_sources(ctx, flags.get("--source"));
  if (!chosen) return fatal(ctx.io, chosen.error());
  if (chosen->empty()) return fatal(ctx.io, "no source is registered; see elctl import add");
  auto client = importer_client(*ctx.store);
  if (!client) return fatal(ctx.io, client.error());

  const bool as_json = flags.has("--json");
  Json listed = Json::array();
  bool ok = true;
  for (const Source& source : *chosen) {
    if (!source.settings) return fatal(ctx.io, missing_settings(ctx, source));
    const SourceSettings& settings = *source.settings;
    // The mirror is read as the last run left it: verify fetches nothing.
    auto opened = open_adapter(ctx, settings, *all, std::nullopt, false);
    if (!opened) return fatal(ctx.io, opened.error());
    for (const auto& line : opened->warnings) ctx.io.err << "warning: " << line << '\n';
    ingest::AgeFn age_fn;
    if (settings.kind == P::ImportSourceKind::ProjectRepo) {
      age_fn = make_age_fn(*ctx.store, settings.uuid, *opened->adapter, constants);
    }
    auto report = ingest::verify(*ctx.store, *client, *opened->adapter, writer_config(settings), age_fn, options);
    if (!report) return fatal(ctx.io, source.name + ": " + report.error().what);
    ok = ok && report->ok();
    if (as_json)
      listed.push_back(to_json(source, *report, constants));
    else
      print(ctx.io.out, source, *report, constants);
  }
  if (as_json) ctx.io.out << listed.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
  return ok ? kOk : kFailed;
}

}  // namespace elctl::import_detail
