// Legacy extraction-line importer (setup::import_legacy_line) and the
// forgiving readers under it, on synthetic files in the formats the legacy
// survey found (docs/superpowers/specs/2026-10-03-legacy-extraction-line-
// survey.md). No lab file is used.

#include "pychron/setup/legacy_line.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include "legacy/lite.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"

using namespace pychron;
using namespace pychron::setup;
namespace fs = std::filesystem;

namespace {

struct Tmp {
  fs::path dir = fs::temp_directory_path() /
                 ("pychron-legacy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Tmp() { fs::create_directories(dir); }
  ~Tmp() { fs::remove_all(dir); }
  void write(const fs::path& rel, const std::string& text) const {
    fs::create_directories((dir / rel).parent_path());
    std::ofstream(dir / rel, std::ios::binary) << text;
  }
};

bool has_note(const LegacyLine& l, const std::string& part) {
  return std::any_of(l.notes.begin(), l.notes.end(), [&](const std::string& n) { return n.find(part) != std::string::npos; });
}

std::string all_notes(const LegacyLine& l) {
  std::string s;
  for (const auto& n : l.notes) s += n + "\n";
  return s;
}

const config::ValveConfig* valve(const config::SystemConfig& c, const std::string& name) {
  for (const auto& v : c.valves)
    if (v.name == name) return &v;
  return nullptr;
}

const canvas::ValveElement* drawn_valve(const canvas::Canvas& c, const std::string& name) {
  for (const auto& v : c.valves)
    if (v.name == name) return &v;
  return nullptr;
}

const canvas::Connection* connection(const canvas::Canvas& c, const std::string& a, const std::string& b) {
  for (const auto& k : c.connections)
    if (k.start == a && k.end == b) return &k;
  return nullptr;
}

// --- readers ------------------------------------------------------------------

TEST(LegacyLite, YamlListOfMapsWithFlowListsQuotesAndComments) {
  std::vector<std::string> skipped;
  const auto root = legacy::parse_yaml(
      "# valves\r\n"
      "- name: A   # inlet\r\n"
      "  address: 'Valve 1_9 Set'\r\n"
      "  interlock: [B, C]\r\n"
      "#- name: D\r\n"
      "- name: '209'\r\n"
      "  description: \"it's #1\"\r\n",
      skipped);
  ASSERT_TRUE(root.is_seq());
  ASSERT_EQ(root.seq.size(), 2u);
  EXPECT_EQ(root.seq[0].text("name"), "A");
  EXPECT_EQ(root.seq[0].text("address"), "Valve 1_9 Set");
  const auto* interlock = root.seq[0].get("interlock");
  ASSERT_NE(interlock, nullptr);
  ASSERT_TRUE(interlock->is_seq());
  ASSERT_EQ(interlock->seq.size(), 2u);
  EXPECT_EQ(interlock->seq[1].scalar, "C");
  EXPECT_EQ(root.seq[1].text("name"), "209");
  EXPECT_EQ(root.seq[1].text("description"), "it's #1");
}

TEST(LegacyLite, XmlTextChildrenAttributesAndAMalformedComment) {
  std::vector<std::string> skipped;
  const auto root = legacy::parse_xml(
      "<?xml version=\"1.0\"?>\n<root>\n"
      "  <valve query_state=\"false\">FD<address>F</address></valve>\n"
      "  <!--<<address>312</address>-->\n"
      "  <group>main<manual_valve>MV</manual_valve></group>\n"
      "</root>\n",
      skipped);
  ASSERT_EQ(root.tag, "root");
  ASSERT_EQ(root.children.size(), 2u);
  const auto& v = root.children[0];
  EXPECT_EQ(v.tag, "valve");
  EXPECT_EQ(v.text, "FD");
  EXPECT_EQ(v.attrs.at("query_state"), "false");
  EXPECT_EQ(v.child_text("address"), "F");
  ASSERT_NE(root.children[1].child("manual_valve"), nullptr);
  EXPECT_EQ(root.children[1].child_text("manual_valve"), "MV");
}

TEST(LegacyLite, IniFoldsKeysAndKeepsTheLastValue) {
  const auto ini = legacy::parse_ini(
      "# comment\n[General]\nType = NGXGPActuator\ninvert: True\n; other\n[Communications]\nhost = a\nhost = b\n");
  EXPECT_EQ(ini.at("General").at("type"), "NGXGPActuator");
  EXPECT_EQ(ini.at("General").at("invert"), "True");
  EXPECT_EQ(ini.at("Communications").at("host"), "b");
  EXPECT_EQ(legacy::split_list(" a, ,b ,c"), (std::vector<std::string>{"a", "b", "c"}));
}

// --- importer -------------------------------------------------------------------

// A melbourne-shaped setup: valves.yaml, canvas.yaml with a lower-left box,
// canvas_config.xml, a Qtegra actuator through a class file and an NGX one.
struct YamlLine : ::testing::Test {
  Tmp t;
  void SetUp() override {
    t.write("extractionline/valves.yaml",
            "- name: A\n"
            "  address: Valve 1_1 Set\n"
            "  description: Inlet\n"
            "  interlock: B\n"
            "- name: B\n"
            "  address: Valve 1_2 Set\n"
            "  interlock: [A, Z]\n"
            "  query_state: false\n"
            "- name: C\n"
            "  address: '3'\n"
            "  actuator: ngx\n"
            "#- name: D\n"
            "#  address: 4\n"
            "- name: M1\n"
            "  kind: manual_valve\n"
            "- name: Air\n"
            "  kind: pipette\n"
            "  inner: A\n"
            "  outer: B\n");
    t.write("canvas2D/canvas.yaml",
            "stage:\n"
            "  - name: S1\n"
            "    translation: -26,0\n"
            "    dimension: 55,3\n"
            "valve:\n"
            "  - name: A\n"
            "    translation: -20,5\n"
            "  - name: B\n"
            "    translation: 0,5\n"
            "  - name: X\n"
            "    translation: 10,5\n"
            "gauge:\n"
            "  - name: G1\n"
            "    translation: 20,5\n"
            "hconnection:\n"
            "  - start: A\n"
            "    end: B\n"
            "vconnection:\n"
            "  - start: A\n"
            "    end: S1\n"
            "connection:\n"
            "  - start: B\n"
            "    end: D\n"
            "tee_connection:\n"
            "  - left: A\n"
            "    mid: C\n"
            "    right: B\n");
    t.write("canvas2D/canvas_config.xml",
            "<root><origin>0,0</origin><xview>-50,50</xview><yview>-25,25</yview></root>\n");
    t.write("devices/switch_controller.cfg", "[General]\ntype = QtegraGPActuator\n");
    t.write("devices/QtegraGPActuator.cfg", "[Communications]\ntype = ethernet\nhost = localhost\nport = 1069\n");
    t.write("devices/ngx.cfg",
            "[General]\ntype = NGXGPActuator\ninvert = True\n"
            "[Communications]\ntype = ethernet\nhost = 192.168.0.5\nport = 1099\n");
  }
};

TEST_F(YamlLine, ValvesActuatorsAndInterlocks) {
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;

  ASSERT_EQ(line->valves.size(), 3u);
  EXPECT_EQ(valve(*line, "A")->address, "Valve 1_1 Set");
  EXPECT_EQ(valve(*line, "A")->description, "Inlet");
  EXPECT_EQ(valve(*line, "A")->actuator, "switch_controller");
  EXPECT_EQ(valve(*line, "A")->interlocks, std::vector<std::string>{"B"});
  EXPECT_EQ(valve(*line, "B")->interlocks, std::vector<std::string>{"A"});  // Z is not a valve
  EXPECT_EQ(valve(*line, "C")->address, "3");
  ASSERT_EQ(line->manual_valves.size(), 1u);
  EXPECT_EQ(line->manual_valves[0].name, "M1");
  ASSERT_EQ(line->pipettes.size(), 1u);
  EXPECT_EQ(line->pipettes[0].inner, "A");

  // Qtegra has no driver: a stand-in, saying what it stands in for.
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "sim_valves");
  EXPECT_EQ(line->transports.at("switch_controller").kind, config::TransportKind::Sim);
  EXPECT_NE(made->line_toml.find("QtegraGPActuator at localhost:1069"), std::string::npos);
  // NGX keeps its endpoint.
  EXPECT_EQ(line->drivers.at("ngx").kind, "ngx_valves");
  const auto& ngx = line->transports.at("ngx");
  ASSERT_EQ(ngx.kind, config::TransportKind::Tcp);
  EXPECT_EQ(std::get<config::TcpParams>(ngx.params).host, "192.168.0.5");
  EXPECT_EQ(std::get<config::TcpParams>(ngx.params).port, 1099);

  EXPECT_EQ(made->read, (std::vector<std::string>{"extractionline/valves.yaml", "canvas2D/canvas.yaml",
                                                  "canvas2D/canvas_config.xml", "devices/switch_controller.cfg",
                                                  "devices/QtegraGPActuator.cfg", "devices/ngx.cfg"}));
  const std::string notes = all_notes(*made);
  EXPECT_TRUE(has_note(*made, "query_state not carried over (1 valve: B)")) << notes;
  EXPECT_TRUE(has_note(*made, "interlock with unknown valve Z dropped")) << notes;
  EXPECT_TRUE(has_note(*made, "ngx: invert=True not carried over")) << notes;
  EXPECT_TRUE(has_note(*made, "QtegraGPActuator): no pychron-cpp driver yet")) << notes;
  // The notes are in the file too.
  EXPECT_NE(made->line_toml.find("#   query_state not carried over"), std::string::npos);
}

TEST_F(YamlLine, CanvasIsRescaledWithBoxesFromTheirLowerLeftCorner) {
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto drawing = canvas::load_canvas_from_string(made->canvas_toml, "canvas.toml");
  ASSERT_TRUE(drawing) << drawing.error().what;

  // View box x -50..50 -> 10 px per unit; y up -> y down over -25..25.
  EXPECT_EQ(drawing->canvas.size, (canvas::Size{1000, 500}));
  ASSERT_NE(drawn_valve(*drawing, "A"), nullptr);
  EXPECT_EQ(drawn_valve(*drawing, "A")->pos, (canvas::Point{300, 200}));
  ASSERT_EQ(drawing->stages.size(), 1u);
  // S1: lower-left (-26, 0), 55 x 3 -> centre (1.5, 1.5).
  EXPECT_EQ(drawing->stages[0].pos, (canvas::Point{515, 235}));
  EXPECT_EQ(drawing->stages[0].size, (canvas::Size{550, 30}));
  // C is not drawn by the legacy canvas: placed, not lost.
  EXPECT_NE(drawn_valve(*drawing, "C"), nullptr);
  EXPECT_NE(drawn_valve(*drawing, "M1"), nullptr);
  EXPECT_EQ(drawn_valve(*drawing, "X"), nullptr);
  EXPECT_TRUE(drawing->gauges.empty());

  ASSERT_NE(connection(*drawing, "A", "B"), nullptr);
  EXPECT_EQ(connection(*drawing, "A", "B")->orientation, canvas::Orientation::Horizontal);
  ASSERT_NE(connection(*drawing, "A", "S1"), nullptr);
  EXPECT_EQ(connection(*drawing, "A", "S1")->orientation, canvas::Orientation::Vertical);
  EXPECT_EQ(connection(*drawing, "B", "D"), nullptr);
  ASSERT_EQ(drawing->tees.size(), 1u);
  EXPECT_EQ(drawing->tees[0].mid, "C");

  const std::string notes = all_notes(*made);
  EXPECT_TRUE(has_note(*made, "valve X is not in the valve file; dropped")) << notes;
  EXPECT_TRUE(has_note(*made, "gauge G1 dropped")) << notes;
  EXPECT_TRUE(has_note(*made, "connection B-D names an element that is not drawn")) << notes;
  EXPECT_TRUE(has_note(*made, "placed in a row")) << notes;
}

TEST_F(YamlLine, AnyPartOfTheSetupFolderFindsTheRest) {
  auto made = import_legacy_line(t.dir / "canvas2D");
  ASSERT_TRUE(made) << made.error().what;
  EXPECT_EQ(made->read.front(), "extractionline/valves.yaml");
}

TEST_F(YamlLine, ValvesYamlIsReadBeforeValvesXml) {
  t.write("extractionline/valves.xml", "<root><valve>Q</valve></root>\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  EXPECT_TRUE(has_note(*made, "extractionline/valves.xml ignored")) << all_notes(*made);
  EXPECT_EQ(made->line_toml.find("\"Q\""), std::string::npos);
}

TEST_F(YamlLine, AMissingActuatorFileGetsAStandInNeverAnotherController) {
  fs::remove(t.dir / "devices" / "ngx.cfg");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(valve(*line, "C")->actuator, "ngx");
  EXPECT_EQ(line->drivers.at("ngx").kind, "sim_valves");
  EXPECT_TRUE(has_note(*made, "actuator ngx: devices/ngx.cfg not found; simulated")) << all_notes(*made);
}

// An NMGRL-shaped setup: valves.xml with groups, names as addresses, a
// second actuator without a file, canvas.xml with the <xvidew> misspelling.
TEST(LegacyLineXml, ValvesXmlAndCanvasXml) {
  Tmp t;
  t.write("extractionline/valves.xml",
          "<root>\n"
          "  <group>main\n"
          "    <valve query_state=\"false\">A<actuator>furnace</actuator></valve>\n"
          "    <valve>B<interlock>A</interlock><check_actuation_enabled>True</check_actuation_enabled></valve>\n"
          "  </group>\n"
          "  <!--<<address>312</address>-->\n"
          "  <manual_valve>MV</manual_valve>\n"
          "</root>\n");
  t.write("canvas2D/canvas.xml",
          "<root>\n"
          "  <xvidew>-30,30</xvidew><yview>-20,20</yview>\n"
          "  <valve>A<translation>-5,0</translation></valve>\n"
          "  <valve>B<translation>5,0</translation></valve>\n"
          "  <manual_valve>MV<translation>0,10</translation></manual_valve>\n"
          "  <spectrometer>Obama<translation>-2,-10</translation><dimension>4,2</dimension></spectrometer>\n"
          "  <connection orientation=\"horizontal\"><start>A</start><end>B</end></connection>\n"
          "  <connection><start offset=\"1,0\">B</start><end>Obama</end></connection>\n"
          "</root>\n");
  t.write("devices/switch_controller.cfg",
          "[General]\ntype = AgilentGPActuator\n[Communications]\ntype = serial\nport = /dev/ttyUSB0\nbaudrate = 19200\n");

  auto made = import_legacy_line(t.dir / "extractionline");
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_EQ(line->valves.size(), 2u);
  EXPECT_EQ(valve(*line, "A")->address, "A");  // the name is the address
  EXPECT_EQ(valve(*line, "A")->actuator, "furnace");
  EXPECT_EQ(valve(*line, "B")->actuator, "switch_controller");
  EXPECT_EQ(valve(*line, "B")->interlocks, std::vector<std::string>{"A"});
  ASSERT_EQ(line->manual_valves.size(), 1u);
  EXPECT_NE(made->line_toml.find("AgilentGPActuator at serial /dev/ttyUSB0 @19200"), std::string::npos);

  auto drawing = canvas::load_canvas_from_string(made->canvas_toml, "canvas.toml");
  ASSERT_TRUE(drawing) << drawing.error().what;
  // 60 wide -> clamp(1000/60) = 16.67 px per unit.
  EXPECT_EQ(drawing->canvas.size, (canvas::Size{1000, 667}));
  ASSERT_EQ(drawing->stages.size(), 1u);
  EXPECT_EQ(drawing->stages[0].display_name, "Obama");
  ASSERT_NE(connection(*drawing, "A", "B"), nullptr);
  EXPECT_EQ(connection(*drawing, "A", "B")->orientation, canvas::Orientation::Horizontal);
  EXPECT_EQ(connection(*drawing, "B", "Obama")->orientation, canvas::Orientation::Auto);

  const std::string notes = all_notes(*made);
  EXPECT_TRUE(has_note(*made, "query_state=\"false\" not carried over (1 valve: A)")) << notes;
  EXPECT_TRUE(has_note(*made, "check_actuation_enabled not carried over")) << notes;
  EXPECT_TRUE(has_note(*made, "B-Obama: end offset not carried over")) << notes;
  EXPECT_TRUE(has_note(*made, "devices/furnace.cfg not found")) << notes;
}

// The oldest drawing: pixel positions in valves2D.cfg, y up.
TEST(LegacyLineValves2D, PixelPositionsFlipped) {
  Tmp t;
  t.write("extractionline/valves.yaml", "- name: V1\n  address: 1\n- name: V2\n  address: 2\n");
  t.write("canvas2D/valves2D.cfg",
          "[General]\nwindow_width = 400\nwindow_height = 300\n[Valve-V1]\npos = 100,200\n[Valve-V2]\npos = 150,200\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto drawing = canvas::load_canvas_from_string(made->canvas_toml, "canvas.toml");
  ASSERT_TRUE(drawing) << drawing.error().what;
  EXPECT_EQ(drawing->canvas.size, (canvas::Size{400, 300}));
  EXPECT_EQ(drawn_valve(*drawing, "V1")->pos, (canvas::Point{100, 100}));
  EXPECT_EQ(drawn_valve(*drawing, "V2")->pos, (canvas::Point{150, 100}));
  EXPECT_TRUE(has_note(*made, "valves2D.cfg has positions only")) << all_notes(*made);
  EXPECT_TRUE(has_note(*made, "switch_controller.cfg not found")) << all_notes(*made);
}

TEST(LegacyLineErrors, NoValveFileOrNoFolderIsAConfigError) {
  Tmp t;
  auto none = import_legacy_line(t.dir);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
  EXPECT_NE(none.error().what.find("valves.yaml"), std::string::npos);

  auto missing = import_legacy_line(t.dir / "nope");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);

  t.write("extractionline/valves.yaml", "# every valve commented out\n#- name: A\n");
  auto empty = import_legacy_line(t.dir);
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().kind, ErrorKind::Config);
}

}  // namespace
