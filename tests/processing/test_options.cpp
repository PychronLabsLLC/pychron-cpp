#include "pychron/processing/options.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "pychron/processing/time_series.hpp"
#include "pychron/processing/units.hpp"

namespace pp = pychron::processing;
namespace fs = std::filesystem;

namespace {

pp::SchemaPtr ts() { return pp::time_series_schema(); }

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& tag) {
    path = fs::temp_directory_path() /
           ("pp_options_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" + tag);
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }
};

TEST(Options, DefaultsComeFromTheSchema) {
  pp::Options o(ts());
  EXPECT_EQ(o.get_string("x.kind"), "time");
  EXPECT_EQ(o.get_int("error_bar_nsigma"), 1);
  EXPECT_FALSE(o.get_optional_double("x.min"));
  EXPECT_FALSE(o.is_set("x.kind"));
  const auto rows = o.rows("panels");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].get_string("quantity"), "Ar40");
  EXPECT_EQ(rows[0].get_string("fit"), "none");
  EXPECT_TRUE(o.rows("groups").empty());
}

TEST(Options, SetValidates) {
  pp::Options o(ts());
  EXPECT_TRUE(o.set("x.kind", std::string("index")));
  EXPECT_EQ(o.get_string("x.kind"), "index");
  EXPECT_FALSE(o.set("x.kind", std::string("sideways")));
  EXPECT_FALSE(o.set("error_bar_nsigma", std::int64_t{7}));
  EXPECT_FALSE(o.set("error_bar_nsigma", std::string("1")));
  EXPECT_TRUE(o.set("error_bar_nsigma", 2.0));  // integral doubles convert
  EXPECT_EQ(o.get_int("error_bar_nsigma"), 2);
  EXPECT_FALSE(o.set("background", std::string("#12345")));
  EXPECT_TRUE(o.set("background", std::string("#a0b0c0")));
  EXPECT_FALSE(o.set("no.such.key", true));
  EXPECT_TRUE(o.set("x.min", 10.0));
  EXPECT_TRUE(o.set("x.min", std::monostate{}));
  EXPECT_FALSE(o.get_optional_double("x.min"));
  EXPECT_FALSE(o.set("x.kind", std::monostate{}));  // required field

  auto row = o.new_row("panels");
  EXPECT_TRUE(row.set("quantity", std::string("Ar40.ic_corrected")));
  EXPECT_EQ(row.get_string("quantity"), "Ar40");  // canonical form
  EXPECT_FALSE(row.set("quantity", std::string("Ar40.bad")));
  EXPECT_TRUE(o.set_rows("panels", {row, row}));
  EXPECT_EQ(o.rows("panels").size(), 2u);
  EXPECT_FALSE(o.set_rows("panels", {}));  // at least one panel
}

TEST(Options, TomlRoundTrip) {
  pp::Options o(ts());
  ASSERT_TRUE(o.set("x.kind", std::string("relative")));
  ASSERT_TRUE(o.set("x.min", -5.5));
  ASSERT_TRUE(o.set("title", std::string("Air \"quoted\"")));
  auto row = o.new_row("panels");
  ASSERT_TRUE(row.set("quantity", std::string("Ar40/Ar36")));
  ASSERT_TRUE(row.set("fit", std::string("linear")));
  ASSERT_TRUE(row.set("y_max", 300.0));
  ASSERT_TRUE(o.set_rows("panels", {row, o.new_row("panels")}));
  const std::string text = pp::options_to_toml(o, "My Air");
  EXPECT_NE(text.find("schema = \"figure.time_series\""), std::string::npos);
  EXPECT_NE(text.find("[x]"), std::string::npos) << text;
  auto back = pp::options_from_toml(ts(), text);
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_TRUE(back->warnings.empty());
  EXPECT_EQ(back->name, "My Air");
  EXPECT_EQ(back->options, o);
  EXPECT_EQ(back->options.rows("panels")[0].get_optional_double("y_max"), 300.0);
}

TEST(Options, UnknownKeysAreKeptAndBadValuesDropped) {
  const char* text = R"(
schema = "figure.time_series"
version = 1
future_option = 3
error_bar_nsigma = 9
[x]
kind = "index"
colour_scheme = "dark"
[[panels]]
quantity = "Ar39"
sparkle = true
[[mystery]]
a = 1
)";
  auto loaded = pp::options_from_toml(ts(), text);
  ASSERT_TRUE(loaded) << loaded.error().what;
  const auto& o = loaded->options;
  EXPECT_EQ(o.get_string("x.kind"), "index");
  EXPECT_EQ(o.get_int("error_bar_nsigma"), 1);  // 9 was out of range
  EXPECT_EQ(loaded->warnings.size(), 1u);
  EXPECT_EQ(o.extra.count("future_option"), 1u);
  EXPECT_EQ(o.extra.count("x.colour_scheme"), 1u);
  EXPECT_EQ(o.rows("panels")[0].extra.count("sparkle"), 1u);
  EXPECT_EQ(o.extra_lists.count("mystery"), 1u);
  // Written back unchanged.
  auto again = pp::options_from_toml(ts(), pp::options_to_toml(o));
  ASSERT_TRUE(again);
  EXPECT_EQ(again->options, o);
}

TEST(Options, WrongSchemaAndSyntaxFail) {
  EXPECT_FALSE(pp::options_from_toml(ts(), "schema = \"figure.ideogram\"\n"));
  EXPECT_FALSE(pp::options_from_toml(ts(), "x = [\n"));
}

TEST(Options, MigrationsRunFromTheFileVersion) {
  auto s = std::make_shared<pp::Schema>();
  s->kind = "test.migrate";
  s->version = 3;
  pp::FieldSpec f;
  f.key = "width";
  f.type = pp::FieldType::Double;
  f.default_value = 1.0;
  s->fields = {f};
  s->migrations = {
      [](pp::Options& o) {  // 1 -> 2: "w" renamed to "width"
        auto& v = o.raw_values();
        if (o.extra.count("w")) {
          v["width"] = o.extra["w"];
          o.extra.erase("w");
        }
      },
      [](pp::Options& o) {  // 2 -> 3: width doubled
        auto& v = o.raw_values();
        if (auto it = v.find("width"); it != v.end()) it->second = std::get<double>(it->second) * 2;
      }};
  pp::SchemaPtr sp = s;
  auto v1 = pp::options_from_toml(sp, "version = 1\nw = 2.5\n");
  ASSERT_TRUE(v1);
  EXPECT_DOUBLE_EQ(v1->options.get_double("width"), 5.0);
  auto v3 = pp::options_from_toml(sp, "version = 3\nwidth = 2.5\n");
  ASSERT_TRUE(v3);
  EXPECT_DOUBLE_EQ(v3->options.get_double("width"), 2.5);
  auto v9 = pp::options_from_toml(sp, "version = 9\nwidth = 2.5\n");
  ASSERT_TRUE(v9);
  EXPECT_FALSE(v9->warnings.empty());
}

TEST(Options, FactoryPresetsAreClean) {
  pp::PresetStore store(fs::temp_directory_path() / "pp_no_such_dir");
  for (const auto& [name, _] : ts()->factory_presets) {
    auto f = store.factory(ts(), name);
    ASSERT_TRUE(f) << name;
    EXPECT_TRUE(f->warnings.empty()) << name;  // legacy W4: factory sets naming missing fields
  }
  // Every unit's schema defaults are valid values.
  for (const auto& kind : pp::UnitRegistry::builtin().kinds()) {
    const auto& schema = pp::UnitRegistry::builtin().find(kind)->schema();
    for (const auto& field : schema->fields) {
      if (std::holds_alternative<std::monostate>(field.default_value)) continue;
      EXPECT_TRUE(pp::validate(field, field.default_value)) << kind << "." << field.key;
    }
  }
}

TEST(Options, PresetStoreLayers) {
  TempDir user("user"), lab("lab");
  pp::PresetStore store(user.path, lab.path);
  auto names = [&] {
    std::vector<std::string> out;
    for (const auto& p : store.list(ts())) out.push_back(p.name);
    return out;
  };
  EXPECT_EQ(names(), (std::vector<std::string>{"Air monitor", "Blanks", "Default", "Spectrometer", "Unknowns"}));

  // A lab preset, then a user preset that shadows a factory one.
  {
    pp::Options lab_opts(ts());
    ASSERT_TRUE(lab_opts.set("title", std::string("lab")));
    fs::create_directories(lab.path / "figure.time_series");
    std::ofstream(lab.path / "figure.time_series" / "lab_air.toml") << pp::options_to_toml(lab_opts, "Lab Air");
  }
  pp::Options mine(ts());
  ASSERT_TRUE(mine.set("title", std::string("mine")));
  ASSERT_TRUE(store.save("Default", mine));
  ASSERT_TRUE(store.save("My Blanks", mine));
  const auto list = store.list(ts());
  ASSERT_EQ(list.size(), 7u);
  for (const auto& p : list) {
    if (p.name == "Default") {
      EXPECT_EQ(p.origin, pp::PresetOrigin::User);
      EXPECT_TRUE(p.shadows);
    }
    if (p.name == "Lab Air") { EXPECT_EQ(p.origin, pp::PresetOrigin::Lab); }
  }
  auto d = store.load(ts(), "default");  // names match case-insensitively
  ASSERT_TRUE(d);
  EXPECT_EQ(d->options.get_string("title"), "mine");
  auto factory = store.factory(ts(), "Default");
  ASSERT_TRUE(factory);
  EXPECT_EQ(factory->options.rows("panels").size(), 2u);

  ASSERT_TRUE(store.rename(ts(), "My Blanks", "Blank Watch"));
  auto renamed = store.load(ts(), "Blank Watch");
  ASSERT_TRUE(renamed);
  EXPECT_EQ(renamed->name, "Blank Watch");
  EXPECT_FALSE(store.remove(ts(), "Lab Air"));  // not a user preset
  EXPECT_FALSE(store.remove(ts(), "Air monitor"));
  ASSERT_TRUE(store.remove(ts(), "Default"));
  EXPECT_EQ(store.load(ts(), "Default")->options.get_string("title"), "");  // factory again
  EXPECT_FALSE(store.save("a/b", mine));
  EXPECT_FALSE(store.save("", mine));
}

}  // namespace
