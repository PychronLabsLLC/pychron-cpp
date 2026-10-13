#pragma once

// Terse FieldSpec constructors for the built-in schemas.

#include <string>
#include <vector>

#include "pychron/processing/options.hpp"

namespace pychron::processing::detail {

inline FieldSpec boolean(std::string key, std::string label, std::string section, bool def, std::string help = {}) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Bool;
  f.default_value = def;
  f.help = std::move(help);
  return f;
}

inline FieldSpec integer(std::string key, std::string label, std::string section, std::int64_t def, double min,
                         double max) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Int;
  f.default_value = def;
  f.min = min;
  f.max = max;
  f.step = 1;
  return f;
}

inline FieldSpec number(std::string key, std::string label, std::string section, double def, double min, double max,
                        double step = 0) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Double;
  f.default_value = def;
  f.min = min;
  f.max = max;
  f.step = step;
  return f;
}

// An optional number: unset by default.
inline FieldSpec optional_number(std::string key, std::string label, std::string section, std::string help = {}) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Double;
  f.optional = true;
  f.help = std::move(help);
  return f;
}

inline FieldSpec text(std::string key, std::string label, std::string section, std::string def = {},
                      std::string help = {}) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::String;
  f.default_value = std::move(def);
  f.help = std::move(help);
  return f;
}

inline FieldSpec font(std::string key, std::string label, std::string section, std::string def = {},
                      std::string help = {}) {
  FieldSpec f = text(std::move(key), std::move(label), std::move(section), std::move(def), std::move(help));
  f.type = FieldType::Font;
  return f;
}

inline FieldSpec choice(std::string key, std::string label, std::string section, std::vector<std::string> choices,
                        std::string def = {}) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Enum;
  f.default_value = def.empty() ? choices.front() : std::move(def);
  f.choices = std::move(choices);
  return f;
}

inline FieldSpec color(std::string key, std::string label, std::string section, std::string def) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Color;
  f.default_value = std::move(def);
  return f;
}

// An optional colour: unset means "automatic".
inline FieldSpec optional_color(std::string key, std::string label, std::string section) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Color;
  f.optional = true;
  return f;
}

inline FieldSpec quantity(std::string key, std::string label, std::string section, std::string def) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::Quantity;
  f.default_value = std::move(def);
  return f;
}

inline FieldSpec strings(std::string key, std::string label, std::string section,
                         std::vector<std::string> def = {}, std::string help = {}) {
  FieldSpec f;
  f.key = std::move(key);
  f.label = std::move(label);
  f.section = std::move(section);
  f.type = FieldType::StringList;
  f.default_value = std::move(def);
  f.help = std::move(help);
  return f;
}

inline FieldSpec when(FieldSpec f, std::string condition) {
  f.enabled_when = std::move(condition);
  return f;
}

inline SchemaPtr make_schema(std::string kind, std::string title, std::vector<FieldSpec> fields,
                             std::vector<ListSpec> lists = {}) {
  auto s = std::make_shared<Schema>();
  s->kind = std::move(kind);
  s->title = std::move(title);
  s->fields = std::move(fields);
  s->lists = std::move(lists);
  return s;
}

}  // namespace pychron::processing::detail
