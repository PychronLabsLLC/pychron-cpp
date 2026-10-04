#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pychron/experiment/conditionals/conditional.hpp"

using namespace pychron::experiment;

namespace {

void expect_same(const ConditionalSet& a, const ConditionalSet& b) {
  EXPECT_EQ(a.disable, b.disable);
  ASSERT_EQ(a.items.size(), b.items.size());
  for (size_t i = 0; i < a.items.size(); ++i) {
    const auto& x = a.items[i];
    const auto& y = b.items[i];
    SCOPED_TRACE(x.name);
    EXPECT_EQ(x.name, y.name);
    EXPECT_EQ(x.kind, y.kind);
    EXPECT_EQ(x.check, y.check);
    EXPECT_EQ(x.start, y.start);
    EXPECT_EQ(x.frequency, y.frequency);
    EXPECT_EQ(x.ntrips, y.ntrips);
    EXPECT_EQ(x.window, y.window);
    EXPECT_EQ(x.mapper, y.mapper);
    EXPECT_EQ(x.analysis_types, y.analysis_types);
    EXPECT_EQ(std::memcmp(&x.abbreviated_count_ratio, &y.abbreviated_count_ratio, sizeof(double)), 0);
    EXPECT_EQ(x.action, y.action);
    EXPECT_EQ(x.resume, y.resume);
    EXPECT_EQ(x.truncate, y.truncate);
    EXPECT_EQ(x.terminate, y.terminate);
  }
}

ConditionalSet parsed(const std::string& text) {
  auto s = parse_conditionals(text);
  EXPECT_TRUE(s) << (s ? "" : s.error().what) << "\n" << text;
  return s ? *s : ConditionalSet{};
}

// Items in file order (the writer groups by kind).
ConditionalSet sorted(ConditionalSet s) {
  auto rank = [](ConditionalKind k) {
    int i = 0;
    for (auto f : kFileOrder) {
      if (f == k) return i;
      ++i;
    }
    return i;
  };
  std::stable_sort(s.items.begin(), s.items.end(),
                   [&](const Conditional& a, const Conditional& b) { return rank(a.kind) < rank(b.kind); });
  return s;
}

void round_trip(const ConditionalSet& s) {
  const std::string text = to_toml(s);
  // toml++ hands tables back by name, so only the order within a kind is the file's.
  auto back = parsed(text);
  expect_same(sorted(s), sorted(back));
  EXPECT_EQ(to_toml(back), text);
}

Conditional make(ConditionalKind kind, const std::string& check) {
  Conditional c;
  c.kind = kind;
  c.check = check;
  auto f = finalize(c);
  EXPECT_TRUE(f) << (f ? "" : f.error().what);
  return f ? *f : c;
}

std::string slurp(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST(ConditionalsWriter, EmptySet) { EXPECT_EQ(to_toml(ConditionalSet{}), ""); }

TEST(ConditionalsWriter, DefaultsOmitted) {
  auto s = parsed("[[truncations]]\ncheck = \"Ar40 > 8e5\"\n");
  EXPECT_EQ(to_toml(s), "[[truncations]]\ncheck = \"Ar40 > 8e5\"\n");
}

TEST(ConditionalsWriter, CanonicalOrder) {
  auto s = parsed(R"(
disable = ["system:gauge_high"]
[[post_run]]
action = "run_blank"
check = "Ar40 < $MIN"
analysis_types = ["air", "cocktail"]
[[truncations]]
abbreviated_count_ratio = 0.5
start = 20
check = "Ar40 > 8e5"
name = "big"
)");
  EXPECT_EQ(to_toml(s),
            "disable = [\"system:gauge_high\"]\n\n"
            "[[truncations]]\nname = \"big\"\ncheck = \"Ar40 > 8e5\"\nstart = 20\nabbreviated_count_ratio = 0.5\n\n"
            "[[post_run]]\ncheck = \"Ar40 < $MIN\"\nanalysis_types = [\"air\", \"cocktail\"]\naction = \"run_blank\"\n");
}

TEST(ConditionalsWriter, EveryFieldRoundTrips) {
  using K = ConditionalKind;
  using T = ActionSpec::Type;
  ConditionalSet s;
  s.disable = {"system:a", "queue:b"};
  auto add = [&](Conditional c) {
    auto f = finalize(c);
    ASSERT_TRUE(f) << f.error().what;
    s.items.push_back(*f);
  };
  auto base = [](K kind, const std::string& name) {
    Conditional c;
    c.kind = kind;
    c.name = name;
    c.check = "Ar40 > 1";
    c.ntrips = 3;
    c.window = 10;
    c.mapper = "x + 1000";
    c.analysis_types = {"unknown", "blank"};
    if (fields_of(kind).gating) {
      c.start = 20;
      c.frequency = 5;
    }
    if (fields_of(kind).ratio) c.abbreviated_count_ratio = 0.25;
    return c;
  };
  add(base(K::Truncation, "t"));
  {
    auto c = base(K::Truncation, "tq");
    c.action.type = T::Truncate;
    c.action.quick = true;
    add(c);
  }
  add(base(K::Termination, "term"));
  add(base(K::Cancelation, "canc"));
  add(base(K::Equilibration, "eq"));
  add(base(K::PreRun, "pre"));
  int n = 0;
  for (const char* text : {"truncate", "truncate:quick", "terminate", "cancel", "set_param X=1.5", "run_hook warn",
                           "notify"}) {
    auto c = base(K::Action, "act" + std::to_string(n++));
    c.action = *parse_action(text);
    c.resume = n % 2 == 0;
    add(c);
  }
  for (const char* text : {"skip_next", "skip_n 3", "skip_aliquot", "skip_to_last_in_aliquot", "set_extract 1,2,3",
                           "set_extract 10%,20%", "repeat", "run_blank"}) {
    auto m = base(K::Modification, "mod" + std::to_string(n));
    m.action = *parse_action(text);
    m.truncate = n % 3 == 0;
    m.terminate = n % 3 == 1;
    add(m);
    auto p = base(K::PostRun, "post" + std::to_string(n++));
    p.action = *parse_action(text);
    add(p);
  }
  round_trip(s);
}

TEST(ConditionalsWriter, EscapesStrings) {
  ConditionalSet s;
  s.disable = {"sys\"tem:a\\b"};
  auto c = make(ConditionalKind::Truncation, "Ar40 > 1");
  c.name = "a\"b\\c\td\ne";
  s.items.push_back(c);
  round_trip(s);
}

TEST(ConditionalsWriter, DoublesRoundTrip) {
  ConditionalSet s;
  int n = 0;
  for (double ratio : {0.1, 0.25, 1e-3, 1.0 / 3.0}) {
    auto c = make(ConditionalKind::Truncation, "Ar40 > " + std::to_string(++n));
    c.abbreviated_count_ratio = ratio;
    s.items.push_back(c);
  }
  for (double v : {5e-9, 8e5, 3.0, 0.1 + 0.2}) {
    Conditional c;
    c.kind = ConditionalKind::Action;
    c.check = "Ar40 > " + std::to_string(++n);
    c.action.type = ActionSpec::Type::SetParam;
    c.action.name = "X";
    c.action.value = v;
    auto f = finalize(c);
    ASSERT_TRUE(f);
    s.items.push_back(*f);
  }
  round_trip(s);
}

TEST(ConditionalsWriter, ExplicitNameKept) {
  auto c = make(ConditionalKind::Truncation, "Ar40 > 1");
  c.name = "mine";
  ConditionalSet s;
  s.items.push_back(c);
  EXPECT_EQ(to_toml(s), "[[truncations]]\nname = \"mine\"\ncheck = \"Ar40 > 1\"\n");
}

TEST(ConditionalsWriter, LabFilesSurvive) {
  namespace fs = std::filesystem;
  int files = 0;
  for (const auto& e : fs::directory_iterator(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "conditionals")) {
    if (e.path().extension() != ".toml") continue;
    SCOPED_TRACE(e.path().string());
    ++files;
    round_trip(parsed(slurp(e.path())));
  }
  EXPECT_GE(files, 2);
}

// Qt sets the C locale from the environment; numbers must not follow it.
TEST(ConditionalsWriter, NumbersIgnoreTheLocale) {
  const std::string before = std::setlocale(LC_NUMERIC, nullptr);
  const char* comma = nullptr;
  for (const char* name : {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "German_Germany.1252"})
    if (std::setlocale(LC_NUMERIC, name) != nullptr) {
      comma = name;
      break;
    }
  if (comma == nullptr) GTEST_SKIP() << "no comma-decimal locale installed";
  ConditionalSet s;
  auto t = make(ConditionalKind::Truncation, "Ar40 > 1");
  t.abbreviated_count_ratio = 0.5;
  s.items.push_back(t);
  auto action = parse_action("set_param X=1.5");
  const std::string text = to_toml(s);
  std::setlocale(LC_NUMERIC, before.c_str());
  EXPECT_NE(text.find("abbreviated_count_ratio = 0.5\n"), std::string::npos) << text;
  ASSERT_TRUE(action) << action.error().what;
  EXPECT_EQ(action->value, 1.5);
  std::setlocale(LC_NUMERIC, comma);
  const std::string written = to_string(*action);
  std::setlocale(LC_NUMERIC, before.c_str());
  EXPECT_EQ(written, "set_param X=1.5");
}
