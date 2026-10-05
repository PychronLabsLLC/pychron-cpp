// codec::pychron_tx: legacy Pychron's valve service, as base_valve.py answers.
#include "pychron/codecs/pychron_tx.hpp"

#include <gtest/gtest.h>

using namespace pychron;
namespace tx = pychron::codec::pychron_tx;

TEST(PychronTxCodec, CommandsAndFraming) {
  EXPECT_EQ(*tx::open_valve("A"), (codec::Command{to_bytes("Open A\r"), ReadSpec::until_close()}));
  EXPECT_EQ(tx::close_valve(" Inlet ")->tx, to_bytes("Close Inlet\r"));
  EXPECT_EQ(tx::get_valve_state("A")->tx, to_bytes("GetValveState A\r"));
  for (const char* bad : {"", "  ", "A,B", "A\rB"}) {
    auto r = tx::open_valve(bad);
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
  }
}

TEST(PychronTxCodec, Actuation) {
  EXPECT_TRUE(tx::decode_actuation(to_bytes("OK")));
  EXPECT_TRUE(tx::decode_actuation(to_bytes("ok")));  // it already was
  auto lock = tx::decode_actuation(to_bytes("ERROR 014 : Valve A is software locked"));
  ASSERT_FALSE(lock);
  EXPECT_EQ(lock.error().kind, ErrorKind::Interlock);
  EXPECT_NE(lock.error().what.find("ERROR 014: Valve A is software locked"), std::string::npos) << lock.error().what;
  EXPECT_EQ(tx::decode_actuation(to_bytes("ERROR 012 : Access restricted to A (x). You are y")).error().kind,
            ErrorKind::Interlock);
  EXPECT_EQ(tx::decode_actuation(to_bytes("ERROR 005 : Z is not a registered valve name")).error().kind,
            ErrorKind::Config);
  EXPECT_EQ(tx::decode_actuation(to_bytes("ERROR 015 : Valve A failed to actuate open")).error().kind,
            ErrorKind::Protocol);
  for (const char* bad : {"", "True", "No Response", "OKAY"}) {
    auto r = tx::decode_actuation(to_bytes(bad));
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    EXPECT_TRUE(r.error().device.empty());
  }
}

TEST(PychronTxCodec, State) {
  EXPECT_EQ(*tx::decode_state(to_bytes("OK")), true);  // Python True, sent as OK
  EXPECT_EQ(*tx::decode_state(to_bytes("True")), true);
  EXPECT_EQ(*tx::decode_state(to_bytes("False")), false);
  EXPECT_EQ(tx::decode_state(to_bytes("ERROR 005 : Z is not a registered valve name")).error().kind,
            ErrorKind::Config);
  for (const char* bad : {"", "No Response", "ok", "0", "closed"}) {
    auto r = tx::decode_state(to_bytes(bad));
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
}

TEST(PychronTxCodec, ServerSide) {
  auto r = tx::decode_request(to_bytes("GetValveState Inlet\r"));
  ASSERT_TRUE(r);
  EXPECT_EQ(r->verb, "GetValveState");
  EXPECT_EQ(r->name, "Inlet");
  EXPECT_FALSE(tx::decode_request(to_bytes("\r\n")));
  EXPECT_EQ(tx::encode_ok(true), to_bytes("OK"));
  EXPECT_EQ(tx::encode_ok(false), to_bytes("ok"));
  EXPECT_EQ(tx::encode_state(false), to_bytes("False"));
  EXPECT_EQ(tx::encode_error("005", "Z is not a registered valve name"),
            to_bytes("ERROR 005 : Z is not a registered valve name"));
}
