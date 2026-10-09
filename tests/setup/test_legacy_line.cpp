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
#include <optional>
#include <random>
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
  // ctest runs each test in its own process, several at once: the clock
  // alone can name two of them the same directory.
  fs::path dir = fs::temp_directory_path() /
                 ("pychron-legacy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                  std::to_string(std::random_device{}()));
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
      "  <turbo display_name='Turbo'>FATurbo\n    <color>0,255,208</color>FATurbo\n  </turbo>\n"
      "</root>\n",
      skipped);
  ASSERT_EQ(root.tag, "root");
  ASSERT_EQ(root.children.size(), 3u);
  // text after a child (a stray repeat in a real file) is not the element's text
  EXPECT_EQ(root.children[2].text, "FATurbo");
  EXPECT_EQ(root.children[2].attrs.at("display_name"), "Turbo");
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
            "    end:\n"
            "      name: S1\n"
            "      offset: 4,3\n"
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

  // Qtegra valves on the legacy endpoint; no kind= means UDP, as in legacy.
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "qtegra_valves");
  const auto& qtegra = line->transports.at("switch_controller");
  ASSERT_EQ(qtegra.kind, config::TransportKind::Udp);
  EXPECT_EQ(std::get<config::UdpParams>(qtegra.params).host, "localhost");
  EXPECT_EQ(std::get<config::UdpParams>(qtegra.params).port, 1069);
  EXPECT_NE(made->line_toml.find("QtegraGPActuator at localhost:1069 (udp)"), std::string::npos);
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
  EXPECT_TRUE(has_note(*made, "switch_controller: no kind= in the device file; written as udp")) << notes;
  EXPECT_TRUE(has_note(*made, "Qtegra takes one client")) << notes;
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
  // A: lower-left (-20, 5), valve_dimension 2 x 2 -> center (-19, 6).
  EXPECT_EQ(drawn_valve(*drawing, "A")->pos, (canvas::Point{310, 190}));
  ASSERT_EQ(drawing->stages.size(), 1u);
  // S1: lower-left (-26, 0), 55 x 3 -> center (1.5, 1.5).
  EXPECT_EQ(drawing->stages[0].pos, (canvas::Point{515, 235}));
  EXPECT_EQ(drawing->stages[0].size, (canvas::Size{550, 30}));
  // C is not drawn by the legacy canvas: placed, not lost.
  EXPECT_NE(drawn_valve(*drawing, "C"), nullptr);
  EXPECT_NE(drawn_valve(*drawing, "M1"), nullptr);
  EXPECT_EQ(drawn_valve(*drawing, "X"), nullptr);
  // a gauge is kept, for illustration: G1 at (20, 5), no dimension
  ASSERT_EQ(drawing->gauges.size(), 1u);
  EXPECT_EQ(drawing->gauges[0].name, "G1");
  EXPECT_EQ(drawing->gauges[0].pos, (canvas::Point{700, 200}));

  ASSERT_NE(connection(*drawing, "A", "B"), nullptr);
  EXPECT_EQ(connection(*drawing, "A", "B")->orientation, canvas::Orientation::Horizontal);
  ASSERT_NE(connection(*drawing, "A", "S1"), nullptr);
  EXPECT_EQ(connection(*drawing, "A", "S1")->orientation, canvas::Orientation::Vertical);
  // The end meets S1 4 right and 3 up from its lower-left corner (its top
  // edge): from its center (27.5, 1.5), that is 23.5 left and 1.5 up.
  EXPECT_EQ(connection(*drawing, "A", "S1")->end_offset, (canvas::Point{-235, -15}));
  EXPECT_EQ(connection(*drawing, "A", "S1")->start_offset, (canvas::Point{0, 0}));
  EXPECT_EQ(connection(*drawing, "B", "D"), nullptr);
  ASSERT_EQ(drawing->tees.size(), 1u);
  EXPECT_EQ(drawing->tees[0].mid, "C");

  const std::string notes = all_notes(*made);
  EXPECT_TRUE(has_note(*made, "valve X is not in the valve file; dropped")) << notes;
  EXPECT_TRUE(has_note(*made, "1 gauges drawn for illustration, with no reading")) << notes;
  EXPECT_TRUE(has_note(*made, ": G1")) << notes;
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

TEST_F(YamlLine, AQtegraActuatorKeepsATcpKind) {
  // melbourne's file says kind = TCP; a lab-named class still means Qtegra.
  t.write("devices/switch_controller.cfg", "[General]\ntype = ObamaQtegraGPActuator\n");
  t.write("devices/ObamaQtegraGPActuator.cfg",
          "[Communications]\ntype = ethernet\nkind = TCP\nhost = 10.0.0.2\nport = 1069\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "qtegra_valves");
  ASSERT_EQ(line->transports.at("switch_controller").kind, config::TransportKind::Tcp);
  EXPECT_FALSE(has_note(*made, "switch_controller: no kind=")) << all_notes(*made);
}

TEST_F(YamlLine, AnotherPychronsValvesAreAddressedByName) {
  // jan drives felix's valves: legacy sent each valve's name, not its address.
  t.write("devices/switch_controller.cfg",
          "[General]\ntype = PychronGPActuator\n[Communications]\ntype = ethernet\nkind = TCP\nhost = felix.local\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "pychron_valves");
  const auto& tr = line->transports.at("switch_controller");
  ASSERT_EQ(tr.kind, config::TransportKind::Tcp);
  EXPECT_EQ(std::get<config::TcpParams>(tr.params).host, "felix.local");
  EXPECT_EQ(std::get<config::TcpParams>(tr.params).port, 1061);
  EXPECT_EQ(valve(*line, "A")->address, "A");  // was "Valve 1_1 Set"
  EXPECT_EQ(valve(*line, "B")->address, "B");
  EXPECT_EQ(valve(*line, "C")->address, "3");  // on another actuator: unchanged
  EXPECT_TRUE(has_note(*made, "2 valve address(es) replaced by the valve name")) << all_notes(*made);
}

// AELAMS's PLC: coils over Modbus TCP, 1-based addresses unchanged (plan
// 2026-10-05, A7).
TEST_F(YamlLine, APlcActuatorIsModbusTcpWithItsAddressesUnchanged) {
  t.write("devices/switch_controller.cfg",
          "[General]\ntype = PLC2000GPActuator\n[Communications]\ntype = ethernet\nhost = 192.168.1.20\n");
  t.write("extractionline/valves.yaml", "- name: A\n  address: 5\n- name: B\n  address: 6\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "plc2000_valves");
  const auto& tr = line->transports.at("switch_controller");
  ASSERT_EQ(tr.kind, config::TransportKind::ModbusTcp);
  EXPECT_EQ(std::get<config::ModbusTcpParams>(tr.params).tcp.host, "192.168.1.20");
  EXPECT_EQ(std::get<config::ModbusTcpParams>(tr.params).tcp.port, 502);
  EXPECT_EQ(valve(*line, "A")->address, "5");  // coil 4 on the wire, as legacy
  EXPECT_TRUE(has_note(*made, "unit id is written as the default 1")) << all_notes(*made);
}

// Agilent units become agilent_switch on the legacy port (plan 2026-10-05, A1).
TEST_F(YamlLine, AnAgilentUnitOnSerialKeepsItsPortAndInvert) {
  // Reston's indirection: switch_controller names the class, whose file has the comms.
  t.write("devices/switch_controller.cfg", "[General]\ntype = AgilentGPActuator\ninvert = True\n");
  t.write("devices/AgilentGPActuator.cfg",
          "[Communications]\ntype = serial\nport = usbserial-FTXYZ\nbaudrate = 19200\nbytesize = 8\nparity = "
          "None\nstopbits = 1\n");
  fs::remove(t.dir / "devices" / "QtegraGPActuator.cfg");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what << "\n" << made->line_toml;
  const auto& d = line->drivers.at("switch_controller");
  EXPECT_EQ(d.kind, "agilent_switch");
  EXPECT_EQ(d.options["invert"].value<bool>(), true);
  const auto& tr = line->transports.at("switch_controller");
  ASSERT_EQ(tr.kind, config::TransportKind::Serial);
  const auto& sp = std::get<config::SerialParams>(tr.params);
  EXPECT_EQ(sp.port, "/dev/tty.usbserial-FTXYZ");
  EXPECT_EQ(sp.baud, 19200);
  EXPECT_EQ(sp.parity, config::Parity::None);
  EXPECT_TRUE(has_note(*made, "serial port usbserial-FTXYZ written as /dev/tty.usbserial-FTXYZ")) << all_notes(*made);
  EXPECT_FALSE(has_note(*made, "switch_controller: invert=True not carried over")) << all_notes(*made);
  EXPECT_FALSE(has_note(*made, "AgilentGPActuator): no pychron-cpp driver yet")) << all_notes(*made);
}

TEST_F(YamlLine, AnAgilentUnitOnTheLanUsesItsScpiSocket) {
  t.write("devices/switch_controller.cfg",
          "[General]\ntype = AgilentGPActuator\n[Communications]\ntype = ethernet\nhost = 10.0.0.30\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  const auto& tr = line->transports.at("switch_controller");
  ASSERT_EQ(tr.kind, config::TransportKind::Tcp);
  EXPECT_EQ(std::get<config::TcpParams>(tr.params).host, "10.0.0.30");
  EXPECT_EQ(std::get<config::TcpParams>(tr.params).port, 5025);
  EXPECT_FALSE(line->drivers.at("switch_controller").options.contains("invert"));
}

TEST_F(YamlLine, AnAgilentUnitOnVisaUsbIsSimulatedAndSaysWhy) {
  // ASU's unit: owner decision 2026-10-05, no USB transport yet.
  t.write("devices/switch_controller.cfg",
          "[General]\ntype = AgilentGPActuator\n[Communications]\ntype = visa\nboard = 0\nmanufacture_id = 2391\n"
          "model_code = 0x0007\nserial_number = MY123\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto line = config::load_system_config_from_string(made->line_toml, "extraction_line.toml");
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "agilent_switch");
  EXPECT_EQ(line->transports.at("switch_controller").kind, config::TransportKind::Sim);
  EXPECT_TRUE(has_note(*made, "VISA-USB is not supported; simulated")) << all_notes(*made);
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
          "  <spectrometer>Obama<translation>-2,-10</translation><dimension>4,2</dimension>\n"
          "    <connection orientation=\"vertical\"><start>MV</start><end>Obama</end></connection>\n"
          "  </spectrometer>\n"
          "  <laser use_symbol=\"True\" display_name='Laser'>CO2<translation>10,-10</translation><dimension>4,4</dimension></laser>\n"
          "  <laser use_symbol=\"False\" display_name=\"\">Furnace<translation>16,-10</translation><dimension>4,4</dimension></laser>\n"
          "  <turbo use_symbol=\"True\" display_name='Turbo'>T1<translation>10,5</translation><dimension>5,3</dimension></turbo>\n"
          "  <getter>NP10<translation>18,5</translation><dimension>5,3</dimension></getter>\n"
          "  <ionpump use_symbol=\"True\" display_name=\"Ion Pump\">IP<translation>24,5</translation><dimension>5,3</dimension></ionpump>\n"
          "  <tank>Air<translation>24,-10</translation><dimension>4,2</dimension></tank>\n"
          "  <connection orientation=\"horizontal\"><start>A</start><end>B</end></connection>\n"
          "  <connection><start offset=\"1,0\">B</start><end>Obama</end></connection>\n"
          "  <elbow><start>A</start><end>Obama</end></elbow>\n"
          "  <elbow><corner>lr</corner><start>A</start><end>MV</end></elbow>\n"
          "  <elbow corner=\"lr\"><start>B</start><end>MV</end></elbow>\n"
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
  // The Agilent unit is driven on its legacy port, which is already a path.
  EXPECT_EQ(line->drivers.at("switch_controller").kind, "agilent_switch");
  const auto& agilent = line->transports.at("switch_controller");
  ASSERT_EQ(agilent.kind, config::TransportKind::Serial);
  EXPECT_EQ(std::get<config::SerialParams>(agilent.params).port, "/dev/ttyUSB0");
  EXPECT_EQ(std::get<config::SerialParams>(agilent.params).baud, 19200);
  EXPECT_NE(made->line_toml.find("AgilentGPActuator on serial /dev/ttyUSB0"), std::string::npos);

  auto drawing = canvas::load_canvas_from_string(made->canvas_toml, "canvas.toml");
  ASSERT_TRUE(drawing) << drawing.error().what;
  // 60 wide -> clamp(1000/60) = 16.67 px per unit.
  EXPECT_EQ(drawing->canvas.size, (canvas::Size{1000, 667}));
  // A spectrometer and a laser say what they are; a laser whose legacy
  // symbol is turned off is a plain volume.
  auto stage = [&](const std::string& name) -> const canvas::StageElement* {
    for (const auto& s : drawing->stages)
      if (s.name == name) return &s;
    return nullptr;
  };
  ASSERT_EQ(drawing->stages.size(), 7u);
  // What colours a region is what a stage is, symbol or no symbol: a laser
  // drawn as a plain box is still a laser, and a tank is a tank.
  EXPECT_EQ(canvas::source_kind(*stage("Furnace")), canvas::SourceKind::Laser);
  EXPECT_EQ(canvas::source_kind(*stage("Air")), canvas::SourceKind::Tank);
  EXPECT_EQ(canvas::source_kind(*stage("T1")), canvas::SourceKind::Pump);
  EXPECT_EQ(stage("T1")->kind, std::nullopt);  // the symbol already says it
  EXPECT_EQ(stage("IP")->symbol, canvas::StageSymbol::IonPump);
  EXPECT_EQ(stage("T1")->symbol, canvas::StageSymbol::Turbo);
  EXPECT_EQ(stage("T1")->display_name, "Turbo");
  EXPECT_EQ(stage("NP10")->symbol, canvas::StageSymbol::Getter);
  EXPECT_EQ(stage("Obama")->display_name, std::nullopt);  // unset: the name is drawn
  EXPECT_EQ(stage("Obama")->symbol, canvas::StageSymbol::Spectrometer);
  EXPECT_EQ(stage("CO2")->symbol, canvas::StageSymbol::Laser);
  EXPECT_EQ(stage("Furnace")->symbol, canvas::StageSymbol::None);
  // display_name is an attribute: a label other than the name, or "" for none
  EXPECT_EQ(stage("CO2")->display_name, "Laser");
  EXPECT_EQ(stage("Furnace")->display_name, "");
  ASSERT_NE(connection(*drawing, "A", "B"), nullptr);
  EXPECT_EQ(connection(*drawing, "A", "B")->orientation, canvas::Orientation::Horizontal);
  EXPECT_EQ(connection(*drawing, "B", "Obama")->orientation, canvas::Orientation::Auto);
  // a connection written inside the element it joins is still a connection
  ASSERT_NE(connection(*drawing, "MV", "Obama"), nullptr);
  EXPECT_EQ(connection(*drawing, "MV", "Obama")->orientation, canvas::Orientation::Vertical);
  // offset 1,0 on a 2 x 2 valve: the middle of its bottom edge, 1 unit
  // (16.67 px) below its center.
  EXPECT_EQ(connection(*drawing, "B", "Obama")->start_offset, (canvas::Point{0, 17}));

  // Elbows turn where legacy pychron turned them: level with the end, above
  // or below the start; for "lr", level with the start. The corner is named
  // as canvas.toml has it, by its place in the ends' bounding box.
  auto corner_of = [&](const std::string& start, const std::string& end) -> std::optional<canvas::Corner> {
    for (const auto& e : drawing->elbows)
      if (e.start == start && e.end == end) return e.corner;
    return std::nullopt;
  };
  ASSERT_EQ(drawing->elbows.size(), 3u);
  EXPECT_EQ(corner_of("A", "Obama"), canvas::Corner::LowerLeft);   // A is left of and above Obama
  EXPECT_EQ(corner_of("A", "MV"), canvas::Corner::LowerRight);     // lr: across from A, then up to MV
  EXPECT_EQ(corner_of("B", "MV"), canvas::Corner::UpperRight);     // a corner= attribute is not read: the default

  const std::string notes = all_notes(*made);
  EXPECT_TRUE(has_note(*made, "query_state=\"false\" not carried over (1 valve: A)")) << notes;
  EXPECT_TRUE(has_note(*made, "check_actuation_enabled not carried over")) << notes;
  EXPECT_FALSE(has_note(*made, "offset not carried over")) << notes;
  EXPECT_TRUE(has_note(*made, "devices/furnace.cfg not found")) << notes;
}

// A window size that is not a number leaves the default, not a window of 0.
TEST(LegacyLineValves2D, AWindowSizeThatIsNotANumberKeepsTheDefault) {
  Tmp t;
  t.write("extractionline/valves.yaml", "- name: V1\n  address: 1\n");
  t.write("canvas2D/valves2D.cfg", "[General]\nwindow_width = wide\nwindow_height = 300\n[Valve-V1]\npos = 100,200\n");
  auto made = import_legacy_line(t.dir);
  ASSERT_TRUE(made) << made.error().what;
  auto drawing = canvas::load_canvas_from_string(made->canvas_toml, "canvas.toml");
  ASSERT_TRUE(drawing) << drawing.error().what;
  EXPECT_EQ(drawing->canvas.size, (canvas::Size{800, 300}));
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
