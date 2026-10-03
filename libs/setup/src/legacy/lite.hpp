#pragma once

// Forgiving readers for the legacy Pychron setup files the extraction-line
// importer reads (legacy extraction-line survey, 2026-10-03). They read the
// subsets those files use, not the full formats, and skip what they cannot
// read instead of failing, saying so in `skipped`.
//
//   YAML  block mappings and sequences, plain and quoted scalars, flow
//         sequences of scalars ([a, b]); comments; CRLF
//   XML   elements, attributes, text (an element's own text, trimmed),
//         comments (also malformed ones), <?...?> and <!...> skipped
//   INI   [Section] key=value (or key: value), # and ; comments; keys
//         folded to lower case, values trimmed and unquoted; a repeated key
//         keeps the last value

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pychron::setup::legacy {

struct YNode {
  enum class Kind { Null, Scalar, Seq, Map };
  Kind kind = Kind::Null;
  std::string scalar;
  std::vector<YNode> seq;
  // A map's keys and values, in file order (two vectors: a pair holding the
  // still incomplete YNode is not allowed).
  std::vector<std::string> keys;
  std::vector<YNode> values;

  bool is_map() const noexcept { return kind == Kind::Map; }
  bool is_seq() const noexcept { return kind == Kind::Seq; }
  // The value under `key` in a map; nullptr when absent or not a map.
  const YNode* get(std::string_view key) const;
  // The scalar under `key`, or "" (a nested map's "name" for {name: X}).
  std::string text(std::string_view key) const;
};

YNode parse_yaml(std::string_view text, std::vector<std::string>& skipped);

struct XNode {
  std::string tag;
  std::map<std::string, std::string> attrs;
  std::string text;  // the element's own text, trimmed
  std::vector<XNode> children;

  const XNode* child(std::string_view tag) const;
  std::string child_text(std::string_view tag) const;
};

XNode parse_xml(std::string_view text, std::vector<std::string>& skipped);

using Ini = std::map<std::string, std::map<std::string, std::string>>;  // section -> key -> value
Ini parse_ini(std::string_view text);

std::string trim(std::string_view s);
// "a, b" / "a,b" / "a" -> parts, trimmed, empty ones dropped.
std::vector<std::string> split_list(std::string_view s, char sep = ',');

}  // namespace pychron::setup::legacy
