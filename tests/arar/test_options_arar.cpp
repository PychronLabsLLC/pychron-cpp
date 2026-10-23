// The options of the Ar-Ar figures and of the flux window (the neutral
// schemas: tests/processing/test_options.cpp).

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <variant>

#include "pychron/arar/arar_figures.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/arar/flux_view.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/units.hpp"

namespace pp = pychron::processing;

// The program writes "color" (as the data browser's "Color by" and the
// detector table's "Color" do): no label, help text or message of an options
// schema says "colour".
TEST(Options, EverySchemaSaysColor) {
  const std::function<void(const pp::Schema&, const std::string&)> check = [&](const pp::Schema& schema,
                                                                               const std::string& where) {
    for (const auto& f : schema.fields) {
      for (const std::string* text : {&f.label, &f.help, &f.section}) {
        EXPECT_EQ(text->find("olour"), std::string::npos) << where << " " << f.key << ": " << *text;
      }
    }
    for (const auto& l : schema.lists) {
      EXPECT_EQ(l.label.find("olour"), std::string::npos) << where << " " << l.key << ": " << l.label;
      if (l.row) check(*l.row, where + " " + l.key);
    }
  };
  for (const pp::SchemaPtr& schema : {pp::ideogram_schema(), pp::spectrum_schema(), pp::isochron_schema(),
                                      pp::spectrum_isochron_schema(), pp::flux_options_schema()}) {
    ASSERT_TRUE(schema);
    check(*schema, schema->kind);
  }
  // the ideogram's group rows are where an analyst sets a group's color
  bool found = false;
  for (const auto& l : pp::ideogram_schema()->lists) {
    if (!l.row) continue;
    for (const auto& f : l.row->fields) {
      if (f.key == "color") {
        found = true;
        EXPECT_EQ(f.label, "Color");
      }
    }
  }
  EXPECT_TRUE(found);
}

TEST(Options, AValueThatIsNoColorIsRefusedInThoseWords) {
  pp::Options o(pp::ideogram_schema());
  auto set = o.set("background", std::string("not a color"));
  ASSERT_FALSE(set);
  EXPECT_NE(set.error().what.find("expected a color ("), std::string::npos) << set.error().what;
}

// Every unit's schema defaults are valid values: the Ar-Ar domain's registry
// (core's units and the Ar-Ar figures).
TEST(Options, EveryArArUnitsDefaultsAreValid) {
  const auto domain = pychron::arar::make_domain();
  const auto& registry = domain->units();
  for (const auto& kind : registry.kinds()) {
    const auto& schema = registry.find(kind)->schema();
    for (const auto& field : schema->fields) {
      if (std::holds_alternative<std::monostate>(field.default_value)) continue;
      EXPECT_TRUE(pp::validate(field, field.default_value)) << kind << "." << field.key;
    }
  }
}
