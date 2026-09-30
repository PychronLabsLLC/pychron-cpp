// NCD ProXR relay codec. Byte values from the ProXR command set as used by
// pychron's hardware/ncd/relay.py: 254 prefix, 49 = select bank, 0-7 = relay
// off, 8-15 = relay on, 16-23 = relay status, 'U' (0x55) = ack.

#include "pychron/codecs/proxr.hpp"

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::codec;

TEST(ProxrCodec, AddressSplitsIntoBankAndRelay) {
  auto a0 = proxr::relay_address(0);
  ASSERT_TRUE(a0);
  EXPECT_EQ(a0->bank, 1);
  EXPECT_EQ(a0->relay, 0);

  auto a7 = proxr::relay_address(7);
  ASSERT_TRUE(a7);
  EXPECT_EQ(a7->bank, 1);
  EXPECT_EQ(a7->relay, 7);

  auto a8 = proxr::relay_address(8);
  ASSERT_TRUE(a8);
  EXPECT_EQ(a8->bank, 2);
  EXPECT_EQ(a8->relay, 0);

  auto last = proxr::relay_address(255);
  ASSERT_TRUE(last);
  EXPECT_EQ(last->bank, 32);
  EXPECT_EQ(last->relay, 7);
}

TEST(ProxrCodec, AddressOutOfRangeIsConfig) {
  auto high = proxr::relay_address(256);
  ASSERT_FALSE(high);
  EXPECT_EQ(high.error().kind, ErrorKind::Config);

  auto negative = proxr::relay_address(-1);
  ASSERT_FALSE(negative);
  EXPECT_EQ(negative.error().kind, ErrorKind::Config);
}

TEST(ProxrCodec, SelectBankEncoding) {
  auto c = proxr::select_bank(1);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->tx, (Bytes{0xFE, 0x31, 0x01}));
  EXPECT_EQ(c->reply, ReadSpec::fixed(1));

  auto c32 = proxr::select_bank(32);
  ASSERT_TRUE(c32);
  EXPECT_EQ(c32->tx, (Bytes{0xFE, 0x31, 0x20}));
}

TEST(ProxrCodec, SelectBankOutOfRangeIsConfig) {
  EXPECT_EQ(proxr::select_bank(0).error().kind, ErrorKind::Config);
  EXPECT_EQ(proxr::select_bank(33).error().kind, ErrorKind::Config);
}

TEST(ProxrCodec, RelayOnOffStatusEncoding) {
  auto on0 = proxr::relay_on(0);
  ASSERT_TRUE(on0);
  EXPECT_EQ(on0->tx, (Bytes{0xFE, 0x08}));
  EXPECT_EQ(on0->reply, ReadSpec::fixed(1));
  EXPECT_EQ(proxr::relay_on(7)->tx, (Bytes{0xFE, 0x0F}));

  auto off0 = proxr::relay_off(0);
  ASSERT_TRUE(off0);
  EXPECT_EQ(off0->tx, (Bytes{0xFE, 0x00}));
  EXPECT_EQ(off0->reply, ReadSpec::fixed(1));
  EXPECT_EQ(proxr::relay_off(7)->tx, (Bytes{0xFE, 0x07}));

  auto st0 = proxr::read_relay(0);
  ASSERT_TRUE(st0);
  EXPECT_EQ(st0->tx, (Bytes{0xFE, 0x10}));
  EXPECT_EQ(st0->reply, ReadSpec::fixed(1));
  EXPECT_EQ(proxr::read_relay(7)->tx, (Bytes{0xFE, 0x17}));
}

TEST(ProxrCodec, RelayOutOfRangeIsConfig) {
  EXPECT_EQ(proxr::relay_on(8).error().kind, ErrorKind::Config);
  EXPECT_EQ(proxr::relay_off(-1).error().kind, ErrorKind::Config);
  EXPECT_EQ(proxr::read_relay(8).error().kind, ErrorKind::Config);
}

TEST(ProxrCodec, DecodeAck) {
  EXPECT_TRUE(proxr::decode_ack(Bytes{0x55}));
}

TEST(ProxrCodec, DecodeAckRejectsAnythingElse) {
  for (const Bytes& bad : {Bytes{}, Bytes{0x00}, Bytes{0x56}, Bytes{0x55, 0x55}}) {
    auto r = proxr::decode_ack(bad);
    ASSERT_FALSE(r) << escape(bad);
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    EXPECT_TRUE(r.error().device.empty());
  }
  EXPECT_EQ(proxr::decode_ack(Bytes{0x56}).error().what, "expected ack 'U': reply \"V\"");
}

TEST(ProxrCodec, DecodeRelayState) {
  auto off = proxr::decode_relay_state(Bytes{0x00});
  ASSERT_TRUE(off);
  EXPECT_FALSE(*off);

  auto on = proxr::decode_relay_state(Bytes{0x01});
  ASSERT_TRUE(on);
  EXPECT_TRUE(*on);
}

TEST(ProxrCodec, DecodeRelayStateRejectsGarbage) {
  for (const Bytes& bad : {Bytes{}, Bytes{0x02}, Bytes{0x55}, Bytes{0x00, 0x01}}) {
    auto r = proxr::decode_relay_state(bad);
    ASSERT_FALSE(r) << escape(bad);
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
}
