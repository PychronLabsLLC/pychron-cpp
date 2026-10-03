// Interpreted ages in both legacy formats, and frozen productions
// (tests/dvc/fixtures/README.md, sections 5.9 and 6.2).

#include <iterator>
#include <utility>

#include "legacy_json.hpp"
#include "pychron/dvc/legacy_layout.hpp"

namespace pychron::dvc {

namespace ps = pychron::persistence;

namespace {

const Json* member(const Json& object, std::string_view key) {
  if (!object.is_object()) return nullptr;
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

std::optional<double> number(const Json& object, std::string_view key) {
  const Json* j = member(object, key);
  return j ? as_double(*j) : std::nullopt;
}

std::optional<std::string> text(const Json& object, std::string_view key) {
  const Json* j = member(object, key);
  if (!j) return std::nullopt;
  auto t = as_text(*j);
  if (t && t->empty()) return std::nullopt;
  return t;
}

// The entry of preferred_kinds for `attr` ("age", "kca").
const Json* preferred_kind(const Json& where, std::string_view attr) {
  const Json* kinds = member(where, "preferred_kinds");
  if (!kinds || !kinds->is_array()) return nullptr;
  for (const auto& kind : *kinds)
    if (const Json* a = member(kind, "attr"); a && a->is_string() && a->get_ref<const std::string&>() == attr)
      return &kind;
  return nullptr;
}

// "<identifier>" or "<identifier>_<NNNNN>" (a 5-digit counter).
std::string identifier_of_path_key(std::string_view key) {
  if (key.size() > 6 && key[key.size() - 6] == '_') {
    bool digits = true;
    for (const char c : key.substr(key.size() - 5)) digits = digits && c >= '0' && c <= '9';
    if (digits) key.remove_suffix(6);
  }
  return std::string(key);
}

}  // namespace

Result<ParsedInterpretedAge> parse_interpreted_age(std::string_view json, std::string_view path_key) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  const Json& doc = *parsed;
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "interpreted age file is not a JSON object");

  ParsedInterpretedAge out;
  auto name = text(doc, "name");
  if (!name) return fail(ErrorKind::Protocol, "interpreted age has no name");
  out.name = std::move(*name);
  if (const auto uuid = text(doc, "uuid")) out.uuid = ps::Uuid::parse(*uuid);
  out.identifier = text(doc, "identifier").value_or(identifier_of_path_key(path_key));

  // Flat (2018): the values are top-level. Nested: under "preferred".
  const Json* preferred = member(doc, "preferred");
  out.nested = preferred != nullptr && preferred->is_object();
  const Json& where = out.nested ? *preferred : doc;

  auto& value = out.value;
  value.doc_json = dump(doc);
  value.age = number(where, "age");
  value.age_err = number(where, "age_err");
  if (const Json* kind = preferred_kind(where, "age"); kind && number(*kind, "value")) {
    value.age = number(*kind, "value");
    value.age_err = number(*kind, "error");
    value.age_kind = text(*kind, "kind");
  }
  if (const Json* kind = preferred_kind(where, "kca"); kind && number(*kind, "value")) {
    value.kca = number(*kind, "value");
    value.kca_err = number(*kind, "error");
  } else {
    value.kca = number(where, "kca");
    value.kca_err = number(where, "kca_err");
  }
  value.mswd = number(where, "mswd");
  if (const Json* n = member(where, "nanalyses")) value.nanalyses = as_int(*n);

  if (const Json* analyses = member(doc, "analyses"); analyses && !analyses->is_null()) {
    if (!analyses->is_array()) return fail(ErrorKind::Protocol, "interpreted age \"analyses\" is not a list");
    std::size_t index = 0;
    for (const auto& analysis : *analyses) {
      if (!analysis.is_object())
        return fail(ErrorKind::Protocol, "interpreted age analyses[" + std::to_string(index) + "] is not an object");
      ParsedInterpretedAge::Member m;
      if (const auto uuid = text(analysis, "uuid")) m.analysis = ps::Uuid::parse(*uuid);
      m.record_id = text(analysis, "record_id").value_or("");
      m.age = number(analysis, "age");
      m.age_err = number(analysis, "age_err");
      if (m.analysis) {
        ps::InterpretedAgeMember row;
        row.analysis = *m.analysis;
        if (!m.record_id.empty()) row.record_id = m.record_id;
        if (const Json* step = member(analysis, "plateau_step")) row.plateau_step = as_bool(*step);
        row.tag = text(analysis, "tag");
        value.members.push_back(std::move(row));
      } else {
        out.notes.push_back("analyses[" + std::to_string(index) + "] " + m.record_id +
                            " has no uuid; it is in the document but not a member row");
      }
      out.members.push_back(std::move(m));
      ++index;
    }
  }
  if (!nonfinite.empty()) {
    std::string note = "non-finite values read as null:";
    for (const auto& n : nonfinite) note += " " + n.pointer + " " + n.token + ";";
    out.notes.push_back(std::move(note));
  }
  return out;
}

Result<ParsedProduction> parse_frozen_production(std::string_view json, std::string_view irradiation,
                                                 std::string_view level) {
  std::vector<NonFinite> nonfinite;
  auto parsed = parse_legacy(json, &nonfinite);
  if (!parsed) return fail(parsed.error());
  Json doc = std::move(*parsed);
  if (!doc.is_object()) return fail(ErrorKind::Protocol, "production file is not a JSON object");
  // ProductionRatio holds plain doubles, so a NaN cannot be stored as unknown.
  if (!nonfinite.empty())
    return fail(ErrorKind::Protocol, "production value " + nonfinite.front().pointer + " is " + nonfinite.front().token);

  ParsedProduction out;
  out.irradiation = std::string(irradiation);
  out.level = std::string(level);
  out.value.reactor = take_text(doc, "reactor");
  out.value.note = take_text(doc, "note");
  out.name = take_text(doc, "name");
  for (auto it = doc.begin(); it != doc.end();) {
    ps::ProductionRatio ratio;
    ratio.key = it.key();
    if (it->is_array()) {
      // [value, error]
      const auto value = it->size() == 2 ? as_double((*it)[0]) : std::nullopt;
      const auto error = it->size() == 2 ? as_double((*it)[1]) : std::nullopt;
      if (!value || !error)
        return fail(ErrorKind::Protocol, "production ratio \"" + ratio.key + "\" is not [value, error]");
      ratio.value = *value;
      ratio.error = *error;
    } else if (it->is_number()) {
      ratio.value = it->get<double>();
    } else {
      ++it;  // not a ratio: kept in extra
      continue;
    }
    out.value.ratios.push_back(std::move(ratio));
    it = doc.erase(it);
  }
  out.extra_json = extra_text(doc);
  return out;
}

}  // namespace pychron::dvc
