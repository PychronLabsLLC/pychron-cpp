// The setup template language: substitution, the toml filter, conditions,
// loops over lists and table rows, tag-only lines, and its errors.

#include "pychron/setup/template.hpp"

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::setup;

namespace {

const Answers kAnswers{
    {"name", Value{std::string("Argus \"VI\"")}},
    {"port", Value{std::int64_t{1069}}},
    {"hv", Value{4500.0}},
    {"ratio", Value{0.125}},
    {"sim", Value{true}},
    {"off", Value{false}},
    {"empty", Value{std::string{}}},
    {"list", Value{std::vector<std::string>{"H1", "AX"}}},
    {"detectors", Value{std::vector<Row>{{{"name", "H1"}, {"isotope", "Ar40"}}, {{"name", "CDD"}, {"isotope", ""}}}}},
};

std::string R(std::string_view text) {
  auto r = render(text, kAnswers, "t");
  EXPECT_TRUE(r) << (r ? "" : r.error().what);
  return r ? *r : std::string{};
}

}  // namespace

TEST(Template, SubstitutesValuesAsTextAndAsToml) {
  EXPECT_EQ(R("port {{ port }}, hv {{hv}}, r {{ ratio }}"), "port 1069, hv 4500.0, r 0.125");
  EXPECT_EQ(R("{{ list }}"), "H1, AX");
  EXPECT_EQ(R("name = {{ name | toml }}"), "name = \"Argus \\\"VI\\\"\"");
  EXPECT_EQ(R("{{ list | toml }} {{ sim | toml }} {{ port | toml }}"), "[\"H1\", \"AX\"] true 1069");
}

TEST(Template, ConditionsAndLoops) {
  EXPECT_EQ(R("{% if sim %}a{% else %}b{% endif %}"), "a");
  EXPECT_EQ(R("{% if not sim %}a{% else %}b{% endif %}"), "b");
  EXPECT_EQ(R("{% if off %}a{% endif %}"), "");
  EXPECT_EQ(R("{% if empty %}a{% else %}none{% endif %}"), "none");
  EXPECT_EQ(R("{% if port == \"1069\" %}std{% endif %}"), "std");
  EXPECT_EQ(R("{% for x in list %}<{{ x }}>{% endfor %}"), "<H1><AX>");
  EXPECT_EQ(R("{% for d in detectors %}{% if d.isotope != \"\" %}{{ d.isotope }}={{ d.name }};{% endif %}{% endfor %}"),
            "Ar40=H1;");
}

TEST(Template, LinesHoldingOnlyATagLeaveNoBlankLines) {
  const std::string text =
      "[a]\n"
      "{% if sim %}\n"
      "kind = \"sim\"\n"
      "{% else %}\n"
      "kind = \"tcp\"\n"
      "{% endif %}\n"
      "{% for d in detectors %}\n"
      "  {{ d.name }}\n"
      "{% endfor %}\n"
      "end\n";
  EXPECT_EQ(R(text), "[a]\nkind = \"sim\"\n  H1\n  CDD\nend\n");
}

TEST(Template, ErrorsNameTheTemplateAndLine) {
  for (const auto& [text, expected] : std::vector<std::pair<std::string, std::string>>{
           {"a\n{{ nope }}", "t:2: unknown name 'nope'"},
           {"{% if sim %}x", "t:1: {% if %} without {% endif %}"},
           {"{% for x list %}{% endfor %}", "t:1: {% for x in list %} expected"},
           {"{{ name | upper }}", "unknown filter 'upper'"},
           {"{{ port", "unclosed {{"},
           {"{% while %}", "unknown tag 'while'"},
           {"{% if port == 1069 %}{% endif %}", "quoted string"},
           {"{% for x in port %}{% endfor %}", "is not a list"},
           {"{{ d.name }}", "unknown name 'd'"},
       }) {
    auto r = render(text, kAnswers, "t");
    ASSERT_FALSE(r) << text;
    EXPECT_NE(r.error().what.find(expected), std::string::npos) << text << " -> " << r.error().what;
  }
}

TEST(Template, EvaluateIsTheSameConditionLanguage) {
  EXPECT_TRUE(*evaluate("sim", kAnswers));
  EXPECT_FALSE(*evaluate("not sim", kAnswers));
  EXPECT_TRUE(*evaluate("empty == \"\"", kAnswers));
  EXPECT_FALSE(evaluate("missing", kAnswers));
}
