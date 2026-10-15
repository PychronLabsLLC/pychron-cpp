// Each driver's Repeat table, command by command (plan Task 6, spec 4.2): the
// link is taken down for 3 s just after one command reached the instrument,
// so its reply is lost. A Safe command is then sent once more than it is on a
// link that does not drop, and the call succeeds; an Unsafe one is sent once
// and the call returns outcome_unknown.
//
// A new command a driver sends has a row here, or it is not done
// (libs/devices/AGENTS.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/codecs/maxigauge.hpp"
#include "pychron/codecs/modbus.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/agilent_switch.hpp"
#include "pychron/devices/agilent_unit_sim.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/extraction/chromium.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/devices/gp_microion.hpp"
#include "pychron/devices/lakeshore.hpp"
#include "pychron/devices/link.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/devices/plc2000_gauges.hpp"
#include "pychron/devices/plc2000_heater.hpp"
#include "pychron/devices/plc2000_valves.hpp"
#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/devices/proxr_relay.hpp"
#include "pychron/devices/pychron_valve_server_sim.hpp"
#include "pychron/devices/pychron_valves.hpp"
#include "pychron/devices/spectrometer/legacy/adc_bank.hpp"
#include "pychron/devices/spectrometer/legacy/dac_output.hpp"
#include "pychron/devices/spectrometer/legacy/pulse_counter.hpp"
#include "pychron/devices/spectrometer/legacy/serial_hv.hpp"
#include "pychron/devices/spectrometer/ngx.hpp"
#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/devices/varian_xgs600.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "virtual_time.hpp"

namespace pychron {
namespace {

using namespace std::chrono_literals;

using Hit = std::function<bool(const Bytes&)>;

// The tx starts with `prefix`, compared as text.
Hit starts_with(std::string prefix) {
  return [prefix = std::move(prefix)](const Bytes& tx) { return to_string(tx).starts_with(prefix); };
}

// Wraps an instrument's hook. Counts the tx that `hit` matches and, once
// armed, takes the link down for `length` right after the first of them has
// reached the instrument: it acted on the command, and its reply is lost.
class Outage {
 public:
  SimTransport::Hook wrap(SimTransport::Hook inner) {
    return [this, inner = std::move(inner)](const Bytes& tx) {
      Bytes reply = inner(tx);
      std::lock_guard lock(mutex_);
      if (also_ && also_(tx)) ++seen_also_;
      if (hit_ && hit_(tx)) {
        ++seen_;
        if (armed_) {
          armed_ = false;
          transport_->down_for(length_);
        }
      }
      return reply;
    };
  }
  void attach(SimTransport& transport) { transport_ = &transport; }

  void count(Hit hit) {
    std::lock_guard lock(mutex_);
    hit_ = std::move(hit);
    seen_ = 0;
  }
  // A second command to count, never the one the link drops after.
  void count_also(Hit hit) {
    std::lock_guard lock(mutex_);
    also_ = std::move(hit);
    seen_also_ = 0;
  }
  int seen_also() const {
    std::lock_guard lock(mutex_);
    return seen_also_;
  }
  void arm(Hit hit, Duration length = 3s) {
    std::lock_guard lock(mutex_);
    hit_ = std::move(hit);
    seen_ = 0;
    armed_ = true;
    length_ = length;
  }
  int seen() const {
    std::lock_guard lock(mutex_);
    return seen_;
  }

 private:
  mutable std::mutex mutex_;
  SimTransport* transport_ = nullptr;
  Hit hit_;
  Hit also_;
  int seen_ = 0;
  int seen_also_ = 0;
  bool armed_ = false;
  Duration length_{};
};

// The tx starts with `prefix` and is not its query (`prefix` then '?').
Hit command_not_query(std::string prefix) {
  return [prefix = std::move(prefix)](const Bytes& tx) {
    const std::string text = to_string(tx);
    return text.starts_with(prefix) && (text.size() == prefix.size() || text[prefix.size()] != '?');
  };
}

// A Modbus TCP request with function code `function` (byte 7 of the ADU).
Hit modbus(std::uint8_t function) {
  return [function](const Bytes& tx) { return tx.size() > 7 && tx[7] == function; };
}

// A two-byte ProXR relay command whose second byte is in [base, base + 8).
Hit proxr_relay(std::uint8_t base) {
  return [base](const Bytes& tx) {
    return tx.size() == 2 && tx[0] == codec::proxr::kPrefix && tx[1] >= base && tx[1] < base + 8;
  };
}

TransportOptions wire_options(VirtualClock& clock, std::string name) {
  TransportOptions o;
  o.name = std::move(name);
  o.timeout = 100ms;
  o.clock = &clock;
  return o;
}

// One row of a table: what the driver is asked to do (after `setup`), and
// the command it sends that the link drops after.
template <class Rig>
struct Row {
  std::string command;
  Repeat repeat = Repeat::Safe;
  std::function<Result<void>(Rig&)> call;
  std::function<void(Rig&)> setup = {};
  Hit hit = {};  // default: the tx starts with `command`
  // A command sent before it in the same transaction: repeated with it, so
  // seen once more too (the unit repeated is the transaction).
  Hit before = {};
};

template <class T>
Result<void> done(Result<T> r) {
  if (!r) return fail(std::move(r).error());
  return {};
}

// Each row on a fresh rig: once on a link that does not drop, to count how
// often the call sends the command, then with the outage.
template <class Rig>
void check(VirtualClock& clock, const std::vector<Row<Rig>>& rows) {
  for (const auto& row : rows) {
    SCOPED_TRACE(row.command);
    const Hit hit = row.hit ? row.hit : starts_with(row.command);
    int clean = 0;
    int clean_before = 0;
    {
      Rig rig(clock);
      if (row.setup) row.setup(rig);
      rig.outage.count(hit);
      if (row.before) rig.outage.count_also(row.before);
      const auto r = row.call(rig);
      ASSERT_TRUE(r) << "without an outage: " << r.error().what;
      clean = rig.outage.seen();
      clean_before = rig.outage.seen_also();
      ASSERT_GE(clean, 1) << "the call does not send " << row.command;
      if (row.before) {
        ASSERT_GE(clean_before, 1) << "the call does not send what goes before " << row.command;
      }
    }
    Rig rig(clock);
    if (row.setup) row.setup(rig);
    rig.outage.arm(hit);
    if (row.before) rig.outage.count_also(row.before);
    const auto r = row.call(rig);
    if constexpr (requires { rig.verify(); }) rig.verify();
    if (row.repeat == Repeat::Safe) {
      EXPECT_TRUE(r) << r.error().what;
      EXPECT_EQ(rig.outage.seen(), clean + 1) << "a Safe command is sent once more after its reply was lost";
      if (row.before) {
        EXPECT_EQ(rig.outage.seen_also(), clean_before + 1) << "the whole transaction is repeated, not one write";
      }
    } else {
      if (r) {
        ADD_FAILURE() << "an Unsafe command whose reply was lost succeeded";
      } else {
        EXPECT_EQ(r.error().code, kOutcomeUnknown);
      }
      EXPECT_EQ(rig.outage.seen(), 1) << "an Unsafe command is never sent twice";
    }
  }
}

struct RepeatTable : testing::VirtualTimeTest {
  RepeatTable() : VirtualTimeTest(std::chrono::seconds(60)) {}
  VirtualClock clock;
  Clock::Participant main{clock, "test"};
};

// ---- ChromiumLaser ------------------------------------------------------------

DeviceOptions on_link(VirtualClock& clock, Link& link) {
  DeviceOptions o;
  o.clock = &clock;
  o.link = &link;
  return o;
}

extraction::ChromiumOptions chromium_options() {
  extraction::ChromiumOptions o;
  o.limits_mm = {{{-50, 50}, {-50, 50}, {-50, 50}}};
  return o;
}

// Declared in the order they are built; torn down in reverse.
struct ChromiumRig {
  explicit ChromiumRig(VirtualClock& c)
      : clock(c),
        sim(c),
        wire(SimTransport::hooked(outage.wrap(sim.hook()), wire_options(c, "laser_pc"))),
        link(*wire, c),
        laser("co2", *wire, chromium_options(), on_link(c, link)) {
    outage.attach(*wire);
    EXPECT_TRUE(wire->open());
  }
  VirtualClock& clock;
  Outage outage;
  extraction::ChromiumSim sim;
  std::unique_ptr<SimTransport> wire;
  Link link;
  extraction::ChromiumLaser laser;
};

void enabled(ChromiumRig& r) { ASSERT_TRUE(r.laser.enable()); }

TEST_F(RepeatTable, ChromiumLaser) {
  using R = ChromiumRig;
  const std::vector<Row<R>> rows{
      {"Sys.ID?", Repeat::Safe, [](R& r) { return r.laser.prepare(); }},
      {"Scans.Status_Verbosity", Repeat::Safe, [](R& r) { return r.laser.prepare(); }},
      {"Laser.Status?", Repeat::Safe, [](R& r) { return done(r.laser.tripped_interlocks()); }},
      {"Laser.Interlocks?", Repeat::Safe, [](R& r) { return done(r.laser.tripped_interlocks()); },
       [](R& r) { r.sim.trip_interlock("door"); }},
      {"Laser.Enable 1", Repeat::Safe, [](R& r) { return r.laser.enable(); }},
      {"Laser.Enable?", Repeat::Safe, [](R& r) { return done(r.laser.is_enabled()); }},
      {"Laser.Enable 0", Repeat::Safe, [](R& r) { return r.laser.disable(); }, enabled},
      {"Laser.Output ", Repeat::Safe, [](R& r) { return r.laser.extract(12.5, extraction::ExtractUnits::Percent); },
       enabled},
      {"Laser.Output?", Repeat::Safe, [](R& r) { return r.laser.extract(12.5, extraction::ExtractUnits::Percent); },
       enabled, {}, starts_with("Laser.Output ")},
      {"Laser.Stop", Repeat::Safe, [](R& r) { return r.laser.stop_laser(); }, enabled},
      {"Scans.Stop", Repeat::Safe, [](R& r) { return r.laser.disable(); }, enabled},
      {"Stage.Pos?", Repeat::Safe, [](R& r) { return done(r.laser.position()); }},
      {"Stage.MoveTo", Repeat::Safe, [](R& r) { return r.laser.set_xy(1, 2); }},
      {"Stage.Stop", Repeat::Safe, [](R& r) { return r.laser.stop(); }},
      {"Stage.Status?", Repeat::Safe, [](R& r) { return done(r.laser.moving()); },
       [](R& r) { ASSERT_TRUE(r.laser.set_xy(1, 2)); }},
      {"Scans.MoveTo", Repeat::Safe, [](R& r) { return r.laser.move_to_position("s1", false); },
       [](R& r) { r.sim.add_scan({1000, 2000, 0}); }},
      {"Scans.InPos?", Repeat::Safe, [](R& r) { return done(r.laser.moving()); },
       [](R& r) {
         r.sim.add_scan({1000, 2000, 0});
         ASSERT_TRUE(r.laser.move_to_position("s1", false));
       }},
      {"Laser.Fire", Repeat::Unsafe, [](R& r) { return r.laser.fire_laser(); }, enabled},
  };
  check(clock, rows);
}

TEST_F(RepeatTable, ALostFireIsStoppedOnceTheLinkReturns) {
  ChromiumRig rig(clock);
  ASSERT_TRUE(rig.laser.enable());
  rig.outage.arm(starts_with("Laser.Fire"), 30s);
  const TimePoint lost = clock.now();
  const auto r = rig.laser.fire_laser();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, kOutcomeUnknown);
  // Past the call window (20 s): the Stop was tried for the critical window.
  EXPECT_GE(clock.now() - lost, 30s);
  const auto log = rig.sim.log();
  EXPECT_EQ(std::count(log.begin(), log.end(), "Laser.Fire"), 1);
  ASSERT_FALSE(log.empty());
  // The Stop, then its confirm, are the last the laser PC heard.
  ASSERT_GE(log.size(), 2u);
  EXPECT_EQ(log[log.size() - 2], "Laser.Stop");
  EXPECT_FALSE(rig.sim.firing());
  EXPECT_FALSE(*rig.laser.is_firing());
}

// ---- NgxSpectrometer, NgxValves -------------------------------------------------

// The NGX link reconnects in its own reader (ngx_link.hpp), so these rigs
// have no Link: NgxLink::call repeats.

std::string ngx_link_name() {
  static std::atomic<int> n{0};
  return "repeat_ngx" + std::to_string(++n);
}

std::shared_ptr<spectrometer::NgxSimModel> ngx_model(VirtualClock& clock) {
  auto m = std::make_shared<spectrometer::NgxSimModel>();
  m->clock = &clock;
  m->values = {1.5, 2.5, 3.5, 4.5};
  m->mass = 39.96;
  return m;
}

toml::table ngx_options(const std::string& link) {
  return toml::parse("channels = [\"H1\", \"AX\", \"L1\", \"CDD\"]\nlink = \"" + link +
                     "\"\nuser = \"pychron\"\npassword = \"s3cret\"\ncommand_timeout_ms = 1000\n")
      .table();
}

template <class Driver>
struct NgxRig {
  explicit NgxRig(VirtualClock& c)
      : model(ngx_model(c)),
        wire(SimTransport::hooked(outage.wrap(spectrometer::ngx_sim_hook(model)), wire_options(c, "ngx"),
                                  spectrometer::ngx_sim_events(model))),
        options(ngx_options(ngx_link_name())) {
    outage.attach(*wire);
    EXPECT_TRUE(wire->open());
    auto made = Driver::create(DriverArgs{"ngx", *wire, options, &c});
    EXPECT_TRUE(made) << made.error().what;
    if (made) driver = std::move(*made);
  }
  // An actuation outside SAB 1 .. SAB 0 is never sent, repeat or not.
  void verify() {
    std::lock_guard lock(model->mutex);
    EXPECT_EQ(model->unbracketed_actuations, 0);
  }
  Outage outage;
  std::shared_ptr<spectrometer::NgxSimModel> model;
  std::unique_ptr<SimTransport> wire;
  toml::table options;
  std::unique_ptr<Driver> driver;
};

TEST_F(RepeatTable, NgxSpectrometer) {
  using R = NgxRig<spectrometer::NgxSpectrometer>;
  const auto connected = [](R& r) { ASSERT_TRUE(r.driver->connect()); };
  const std::vector<Row<R>> rows{
      {"Login", Repeat::Safe, [](R& r) { return r.driver->connect(); }},
      {"StopAcq", Repeat::Safe, [](R& r) { return r.driver->connect(); }},
      {"SetAcqPeriod", Repeat::Safe, [](R& r) { return r.driver->connect(); }},
      {"SetMass", Repeat::Safe, [](R& r) { return r.driver->set(40.0); }, connected},
      {"GETMASS", Repeat::Safe, [](R& r) { return done(r.driver->read()); }, connected},
      {"SSO", Repeat::Safe, [](R& r) { return r.driver->set_hv(4500); }, connected},
      {"GSO", Repeat::Safe, [](R& r) { return done(r.driver->read_hv()); }, connected},
      {"StartAcq", Repeat::Unsafe, [](R& r) { return r.driver->trigger(); },
       [](R& r) {
         ASSERT_TRUE(r.driver->connect());
         ASSERT_TRUE(r.driver->start());
       }},
  };
  check(clock, rows);
}

TEST_F(RepeatTable, NgxValves) {
  using R = NgxRig<spectrometer::NgxValves>;
  const ValveAddress valve{"3"};
  const auto connected = [](R& r) { ASSERT_TRUE(r.driver->connect()); };
  const std::vector<Row<R>> rows{
      {"Login", Repeat::Safe, [](R& r) { return r.driver->connect(); }},
      {"SAB 1", Repeat::Safe, [&](R& r) { return r.driver->open(valve); }, connected},
      {"OpenValve", Repeat::Safe, [&](R& r) { return r.driver->open(valve); }, connected, {}, starts_with("SAB 1")},
      {"CloseValve", Repeat::Safe, [&](R& r) { return r.driver->close(valve); }, connected, {}, starts_with("SAB 1")},
      {"SAB 0", Repeat::Safe, [&](R& r) { return r.driver->close(valve); }, connected, {}, starts_with("CloseValve")},
      {"GetValveStatus", Repeat::Safe, [&](R& r) { return done(r.driver->read(valve)); }, connected},
  };
  check(clock, rows);
}

// ---- drivers over a plain transport ------------------------------------------------
//
// Each rig: the instrument's simulator, the outage around its hook, the wire,
// a Link made by hand (the line hands them out in Task 7), and the driver on
// that link. Members in that order; torn down in reverse.

std::unique_ptr<SimTransport> wire_for(Outage& outage, SimTransport::Hook hook, VirtualClock& clock, std::string name) {
  auto wire = SimTransport::hooked(outage.wrap(std::move(hook)), wire_options(clock, std::move(name)));
  outage.attach(*wire);
  EXPECT_TRUE(wire->open());
  return wire;
}

template <bool Invert>
struct AgilentRig {
  explicit AgilentRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  AgilentUnitSim unit;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, unit.hook(), clock, "agilent");
  Link link{*wire, clock};
  AgilentSwitch driver{"switch", *wire, Invert, on_link(clock, link)};
};

TEST_F(RepeatTable, AgilentSwitch) {
  const ValveAddress valve{"101"};
  using R = AgilentRig<false>;
  const std::vector<Row<R>> rows{
      {"*IDN?", Repeat::Safe, [](R& r) { return r.driver.connect(); }},
      {"*CLS", Repeat::Safe, [](R& r) { return r.driver.connect(); }},
      {"ROUT:OPEN", Repeat::Safe, [&](R& r) { return r.driver.open(valve); }, {}, command_not_query("ROUT:OPEN")},
      {"ROUT:CLOSE", Repeat::Safe, [&](R& r) { return r.driver.close(valve); }, {}, command_not_query("ROUT:CLOSE")},
      {"ROUT:OPEN?", Repeat::Safe, [&](R& r) { return done(r.driver.read(valve)); }},
      {"SYST:ERR?", Repeat::Unsafe, [&](R& r) { return r.driver.open(valve); }},
  };
  check(clock, rows);

  using I = AgilentRig<true>;
  const std::vector<Row<I>> inverted{
      {"ROUT:CLOSE?", Repeat::Safe, [&](I& r) { return done(r.driver.read(valve)); }},
  };
  check(clock, inverted);
}

struct PychronValvesRig {
  explicit PychronValvesRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  PychronValveServerSim server;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, server.hook(), clock, "felix");
  Link link{*wire, clock};
  PychronValves driver{"felix_valves", *wire, on_link(clock, link)};
};

TEST_F(RepeatTable, PychronValves) {
  using R = PychronValvesRig;
  const ValveAddress valve{"A"};
  const std::vector<Row<R>> rows{
      {"Open", Repeat::Safe, [&](R& r) { return r.driver.open(valve); }},
      {"Close", Repeat::Safe, [&](R& r) { return r.driver.close(valve); }},
      {"GetValveState", Repeat::Safe, [&](R& r) { return done(r.driver.read(valve)); }},
  };
  check(clock, rows);
}

struct ProxrRig {
  explicit ProxrRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  ProxrBoardSim board;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, board.hook(), clock, "valve_bus");
  Link link{*wire, clock};
  ProxrRelay driver{"relays", *wire, on_link(clock, link)};
};

TEST_F(RepeatTable, ProxrRelay) {
  namespace px = codec::proxr;
  using R = ProxrRig;
  const ValveAddress valve{"9"};
  const Hit bank = [](const Bytes& tx) { return tx.size() == 3 && tx[0] == px::kPrefix && tx[1] == px::kSelectBank; };
  const std::vector<Row<R>> rows{
      {"bank select", Repeat::Safe, [&](R& r) { return r.driver.open(valve); }, {}, bank},
      {"relay on", Repeat::Safe, [&](R& r) { return r.driver.open(valve); }, {}, proxr_relay(px::kRelayOnBase), bank},
      {"relay off", Repeat::Safe, [&](R& r) { return r.driver.close(valve); }, {}, proxr_relay(px::kRelayOffBase),
       bank},
      {"relay status", Repeat::Safe, [&](R& r) { return done(r.driver.read(valve)); }, {},
       proxr_relay(px::kRelayStatusBase), bank},
  };
  check(clock, rows);
}

ModbusDeviceSim coil_bank(const std::shared_ptr<std::map<std::uint16_t, bool>>& coils) {
  ModbusDeviceSim d;
  d.read_coil = [coils](std::uint16_t a) -> std::optional<bool> {
    auto it = coils->find(a);
    return it == coils->end() ? std::nullopt : std::optional<bool>(it->second);
  };
  d.write_coil = [coils](std::uint16_t a, bool on) {
    auto it = coils->find(a);
    if (it == coils->end()) return false;
    it->second = on;
    return true;
  };
  return d;
}

struct Plc2000ValvesRig {
  explicit Plc2000ValvesRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  std::shared_ptr<std::map<std::uint16_t, bool>> coils =
      std::make_shared<std::map<std::uint16_t, bool>>(std::map<std::uint16_t, bool>{{0, false}, {1, false}});
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, coil_bank(coils).hook(), clock, "plc");
  Link link{*wire, clock};
  Plc2000Valves driver{"plc_valves", *wire, {}, on_link(clock, link)};
};

TEST_F(RepeatTable, Plc2000Valves) {
  using R = Plc2000ValvesRig;
  const ValveAddress valve{"1"};
  const std::vector<Row<R>> rows{
      {"write single coil (on)", Repeat::Safe, [&](R& r) { return r.driver.open(valve); }, {}, modbus(0x05)},
      {"write single coil (off)", Repeat::Safe, [&](R& r) { return r.driver.close(valve); }, {}, modbus(0x05)},
      {"read coils", Repeat::Safe, [&](R& r) { return done(r.driver.read(valve)); }, {}, modbus(0x01)},
  };
  check(clock, rows);
}

toml::table heater_options() {
  return toml::table{{"enable", 2}, {"use_pid", 1}, {"setpoint", 11}, {"readback", 21}};
}

struct Plc2000HeaterRig {
  explicit Plc2000HeaterRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  Plc2000HeaterOptions options = *Plc2000Heater::parse_options(heater_options());
  Plc2000HeaterSim sim{clock, options, 25.0, std::chrono::seconds(60)};
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, sim.hook(), clock, "plc");
  Link link{*wire, clock};
  Plc2000Heater driver{"heater", *wire, options, &clock, &link};
};

TEST_F(RepeatTable, Plc2000Heater) {
  using R = Plc2000HeaterRig;
  const std::vector<Row<R>> rows{
      {"read coils", Repeat::Safe, [](R& r) { return done(r.driver.enabled()); }, {}, modbus(0x01)},
      {"write single coil", Repeat::Safe, [](R& r) { return r.driver.set_enabled(true); }, {}, modbus(0x05)},
      {"read input registers", Repeat::Safe, [](R& r) { return done(r.driver.readback()); }, {}, modbus(0x04)},
      {"write multiple registers", Repeat::Safe, [](R& r) { return r.driver.set_setpoint(450); }, {},
       modbus(0x10)},
  };
  check(clock, rows);
}

template <bool Celsius>
struct LakeshoreRig {
  explicit LakeshoreRig(VirtualClock& c) : clock(c) {}
  static LakeshoreOptions options() {
    LakeshoreOptions o;
    o.units = Celsius ? codec::lakeshore::Units::Celsius : codec::lakeshore::Units::Kelvin;
    o.bands = {{1, 1, 0.0, 10.0}, {1, 2, 10.0, 30.0}, {1, 3, 30.0, 400.0}};
    return o;
  }
  VirtualClock& clock;
  LakeshoreSim sim{clock};
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, sim.hook(), clock, "cryostat");
  Link link{*wire, clock};
  Lakeshore driver{"cryo", *wire, options(), &clock, &link};
};

TEST_F(RepeatTable, Lakeshore) {
  using R = LakeshoreRig<false>;
  const std::vector<Row<R>> rows{
      {"*CLS", Repeat::Safe, [](R& r) { return r.driver.connect(); }},
      {"*IDN?", Repeat::Safe, [](R& r) { return r.driver.connect(); }},
      {"KRDG?", Repeat::Safe, [](R& r) { return done(r.driver.read_temperature("A")); }},
      {"RANGE 1,", Repeat::Safe, [](R& r) { return r.driver.set_setpoint(1, 20.0); }},
      {"SETP 1,", Repeat::Safe, [](R& r) { return r.driver.set_setpoint(1, 20.0); }, {}, {},
       starts_with("RANGE 1,")},
      {"SETP?", Repeat::Safe, [](R& r) { return done(r.driver.setpoint(1)); }},
  };
  check(clock, rows);

  using C = LakeshoreRig<true>;
  const std::vector<Row<C>> celsius{
      {"CRDG?", Repeat::Safe, [](C& r) { return done(r.driver.read_temperature("A")); }},
  };
  check(clock, celsius);
}

struct GpMicroIonRig {
  explicit GpMicroIonRig(VirtualClock& c) : clock(c) {}
  static MicroIonSimModel model() {
    MicroIonSimModel m;
    m.pressure = [](int) -> std::optional<double> { return 1.2e-8; };
    return m;
  }
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, microion_sim_hook(model()), clock, "gauges");
  Link link{*wire, clock};
  GpMicroIon driver{"ig", *wire, 1, {1}, &clock, &link};
};

TEST_F(RepeatTable, GpMicroIon) {
  using R = GpMicroIonRig;
  const std::vector<Row<R>> rows{
      {"#01DS", Repeat::Safe, [](R& r) { return done(r.driver.read_pressure(1)); }},
      {"#01IG1 ON", Repeat::Safe, [](R& r) { return r.driver.set_ion_gauge(true); }},
      {"#01IG1 OFF", Repeat::Safe, [](R& r) { return r.driver.set_ion_gauge(false); }},
  };
  check(clock, rows);
}

struct MaxiGaugeRig {
  explicit MaxiGaugeRig(VirtualClock& c) : clock(c) {}
  static MaxiGaugeSimModel model() {
    MaxiGaugeSimModel m;
    m.pressure = [](int) -> std::optional<double> { return 3.4e-7; };
    return m;
  }
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, maxigauge_sim_hook(model()), clock, "gauges");
  Link link{*wire, clock};
  PfeifferMaxiGauge driver{"maxi", *wire, {1}, &clock, &link};
};

TEST_F(RepeatTable, PfeifferMaxiGauge) {
  using R = MaxiGaugeRig;
  const Hit enq = [](const Bytes& tx) { return tx == Bytes{0x05}; };
  const std::vector<Row<R>> rows{
      {"PR1", Repeat::Safe, [](R& r) { return done(r.driver.read_pressure(1)); }},
      {"ENQ", Repeat::Safe, [](R& r) { return done(r.driver.read_pressure(1)); }, {}, enq, starts_with("PR1")},
      {"PRX", Repeat::Safe, [](R& r) { return done(r.driver.read_all()); }},
      {"UNI", Repeat::Safe, [](R& r) { return done(r.driver.read_units()); }},
  };
  check(clock, rows);
}

ModbusDeviceSim holding_float(float value) {
  const auto words = codec::modbus::encode_float(value, codec::modbus::WordOrder::CDAB);
  ModbusDeviceSim d;
  d.read_holding = [words](std::uint16_t a) -> std::optional<std::uint16_t> {
    if (a > 1) return std::nullopt;
    return words[a];
  };
  return d;
}

struct Plc2000GaugesRig {
  explicit Plc2000GaugesRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, holding_float(5.0e-8F).hook(), clock, "plc");
  Link link{*wire, clock};
  Plc2000Gauges driver{"plc_gauges", *wire, {}, &clock, &link};
};

TEST_F(RepeatTable, Plc2000Gauges) {
  using R = Plc2000GaugesRig;
  const std::vector<Row<R>> rows{
      {"read holding registers", Repeat::Safe, [](R& r) { return done(r.driver.read_pressure(1)); }, {},
       modbus(0x03)},
  };
  check(clock, rows);
}

struct Xgs600Rig {
  explicit Xgs600Rig(VirtualClock& c) : clock(c) {}
  static Xgs600SimModel model() {
    Xgs600SimModel m;
    m.pressure = [](const std::string&) -> std::optional<double> { return 2.5e-9; };
    return m;
  }
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, xgs600_sim_hook(model()), clock, "xgs");
  Link link{*wire, clock};
  VarianXgs600 driver{"xgs", *wire, "00", {"IMG1"}, &clock, &link};
};

TEST_F(RepeatTable, VarianXgs600) {
  using R = Xgs600Rig;
  const std::vector<Row<R>> rows{
      {"#0002UIMG1", Repeat::Safe, [](R& r) { return done(r.driver.read_pressure(1)); }},
  };
  check(clock, rows);
}

// ---- legacy spectrometer front ends ---------------------------------------------------

struct AdcBankRig {
  explicit AdcBankRig(VirtualClock& c) : clock(c) {}
  static spectrometer::AdcSimModel model() {
    spectrometer::AdcSimModel m;
    m.volts = [](std::size_t) -> std::optional<double> { return 1.5; };
    return m;
  }
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, spectrometer::adc_sim_hook(model()), clock, "adc");
  Link link{*wire, clock};
  spectrometer::AdcBank driver{"faradays", *wire, {"H1"}, {100.0, 1, 0, 1.0}, &clock, &link};
};

TEST_F(RepeatTable, AdcBank) {
  using R = AdcBankRig;
  const auto sampled = [](R& r) -> Result<void> {
    if (auto s = r.driver.start(); !s) return s;
    return done(r.driver.next(std::chrono::seconds(1)));
  };
  const std::vector<Row<R>> rows{
      {"read input registers", Repeat::Safe, sampled, {}, modbus(0x04)},
  };
  check(clock, rows);
}

struct PulseCounterRig {
  explicit PulseCounterRig(VirtualClock& c) : clock(c) {}
  static spectrometer::CounterSimModel model() {
    spectrometer::CounterSimModel m;
    m.counts = [](std::size_t) -> std::uint64_t { return 42; };
    return m;
  }
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, spectrometer::counter_sim_hook(model()), clock, "counter");
  Link link{*wire, clock};
  spectrometer::PulseCounter driver{"cdd", *wire, {"CDD"}, 10.0, &clock, &link};
};

TEST_F(RepeatTable, PulseCounter) {
  using R = PulseCounterRig;
  const std::vector<Row<R>> rows{
      // start() reads once to clear what accumulated while stopped.
      {"R", Repeat::Unsafe, [](R& r) { return r.driver.start(); }},
  };
  check(clock, rows);
}

struct SerialHvRig {
  explicit SerialHvRig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, spectrometer::hv_sim_hook({}), clock, "hv");
  Link link{*wire, clock};
  spectrometer::SerialHv driver{"hv", *wire, 10000.0, &clock, &link};
};

TEST_F(RepeatTable, SerialHv) {
  using R = SerialHvRig;
  const spectrometer::ParamId hv{spectrometer::SourceParam::HV};
  const Hit vout = starts_with("VOUT?");
  const Hit vset = starts_with("VSET?");
  const std::vector<Row<R>> rows{
      {"VSET ", Repeat::Safe, [](R& r) { return r.driver.set_hv(4500); }},
      {"VOUT?", Repeat::Safe, [](R& r) { return done(r.driver.read_hv()); }},
      {"VSET?", Repeat::Safe, [&](R& r) { return done(r.driver.read_param(hv)); }},
      {"VOUT? after VSET?", Repeat::Safe, [&](R& r) { return done(r.driver.read_param(hv)); }, {},
       vout, vset},
  };
  check(clock, rows);
}

struct Map215Rig {
  explicit Map215Rig(VirtualClock& c) : clock(c) {}
  VirtualClock& clock;
  Outage outage;
  std::unique_ptr<SimTransport> wire = wire_for(outage, spectrometer::map215_sim_hook({}), clock, "dac");
  Link link{*wire, clock};
  spectrometer::Map215Dac driver{*wire, 0, 10.0, &link};
};

TEST_F(RepeatTable, Map215Dac) {
  using R = Map215Rig;
  const std::vector<Row<R>> rows{
      {"B", Repeat::Safe, [](R& r) { return done(r.driver.write(5.0)); }},
      {"W", Repeat::Safe, [](R& r) { return done(r.driver.write(5.0)); }, {}, {}, starts_with("B")},
  };
  check(clock, rows);
}

}  // namespace
}  // namespace pychron
