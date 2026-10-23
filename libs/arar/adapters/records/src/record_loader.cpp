#include "pychron/arar/record_loader.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/arar/analysis_data.hpp"

namespace pychron::arar {

namespace fs = std::filesystem;
namespace pp = pychron::processing;

namespace {

Result<std::string> read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// What references.toml holds, by identifier (flux) and irradiation.
struct References {
  std::map<std::string, reduction::Flux> flux;
  std::map<std::string, reduction::ProductionRatios> production;
  std::map<std::string, std::vector<reduction::Dose>> chronology;
};

Result<References> parse_references(const fs::path& p) {
  auto text = read_text(p);
  if (!text) return fail(text.error());
  auto parsed = toml::parse(*text, p.string());
  if (!parsed) return fail(ErrorKind::Config, p.string() + ": " + std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();
  References out;
  if (const auto* f = root.get("flux") ? root.get("flux")->as_table() : nullptr) {
    for (const auto& [id, node] : *f) {
      const auto* t = node.as_table();
      if (!t) continue;
      reduction::Flux fl;
      fl.j.value = t->get("j") ? t->get("j")->value<double>().value_or(0.0) : 0.0;
      fl.j.error = t->get("j_err") ? t->get("j_err")->value<double>().value_or(0.0) : 0.0;
      fl.position_jerr = t->get("position_jerr") ? t->get("position_jerr")->value<double>().value_or(0.0) : 0.0;
      out.flux[std::string(id.str())] = fl;
    }
  }
  if (const auto* pr = root.get("production") ? root.get("production")->as_table() : nullptr) {
    for (const auto& [irr, node] : *pr) {
      const auto* t = node.as_table();
      if (!t) continue;
      std::map<std::string, reduction::Measured, std::less<>> rows;
      for (const auto& [k, val] : *t) {
        const auto* arr = val.as_array();
        if (!arr || arr->size() != 2) continue;
        rows[std::string(k.str())] = {(*arr)[0].value<double>().value_or(0.0), (*arr)[1].value<double>().value_or(0.0)};
      }
      auto ratios = reduction::production_from_rows(rows);
      if (!ratios) return fail(ErrorKind::Config, p.string() + ": production " + std::string(irr.str()) + ": " +
                                                      ratios.error().what);
      out.production[std::string(irr.str())] = *ratios;
    }
  }
  if (const auto* ch = root.get("chronology") ? root.get("chronology")->as_table() : nullptr) {
    for (const auto& [irr, node] : *ch) {
      const auto* arr = node.as_array();
      if (!arr) continue;
      std::vector<reduction::Dose> doses;
      for (const auto& e : *arr) {
        const auto* t = e.as_table();
        if (!t) continue;
        reduction::Dose d;
        d.power = t->get("power") ? t->get("power")->value<double>().value_or(1.0) : 1.0;
        const auto start = pp::parse_utc(t->get("start") ? t->get("start")->value<std::string>().value_or("") : "");
        const auto end = pp::parse_utc(t->get("end") ? t->get("end")->value<std::string>().value_or("") : "");
        if (!start || !end)
          return fail(ErrorKind::Config, p.string() + ": chronology " + std::string(irr.str()) + ": bad date");
        d.start_utc_s = static_cast<std::int64_t>(*start);
        d.end_utc_s = static_cast<std::int64_t>(*end);
        doses.push_back(d);
      }
      out.chronology[std::string(irr.str())] = doses;
    }
  }
  return out;
}

// The file as last read. The source calls refresh and load under its own
// lock, one at a time, so this needs none.
struct State {
  fs::path path;
  std::optional<fs::file_time_type> mtime;  // of what `refs` was read from; none: no file
  References refs;

  // Reads the file again when it changed; whether what load gives changed.
  Result<bool> refresh() {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
      const bool had = mtime.has_value();
      refs = {};
      mtime.reset();
      return had;
    }
    const auto now = fs::last_write_time(path, ec);
    if (mtime && *mtime == now) return false;
    auto read = parse_references(path);
    if (!read) return fail(read.error());
    refs = std::move(*read);
    mtime = now;
    return true;
  }

  std::shared_ptr<const pp::DomainAnalysisData> load(const pp::Analysis& a) const {
    auto data = std::make_shared<ArArAnalysisData>();
    if (auto f = refs.flux.find(a.identifier); f != refs.flux.end()) data->flux = f->second;
    if (auto p = refs.production.find(a.irradiation); p != refs.production.end()) data->production = p->second;
    if (auto c = refs.chronology.find(a.irradiation); c != refs.chronology.end()) data->chronology = c->second;
    return data;
  }
};

}  // namespace

pp::RecordExtensionLoader record_extension_loader(const fs::path& references_toml) {
  auto state = std::make_shared<State>();
  state->path = references_toml;
  pp::RecordExtensionLoader loader;
  loader.refresh = [state] { return state->refresh(); };
  loader.load = [state](const experiment::record::AnalysisRecord&, const pp::Analysis& a) { return state->load(a); };
  return loader;
}

}  // namespace pychron::arar
