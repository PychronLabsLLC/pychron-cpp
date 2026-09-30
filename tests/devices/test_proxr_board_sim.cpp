// The ProXR board model SimSystem puts behind a hooked SimTransport.

#include "pychron/devices/proxr_board_sim.hpp"

#include <gtest/gtest.h>

using namespace pychron;

TEST(ProxrBoardSim, StartsDeEnergized) {
  ProxrBoardSim board;
  for (std::int64_t i = 0; i <= 255; ++i) EXPECT_FALSE(board.energized(i)) << i;
  EXPECT_EQ(board.selected_bank(), 1);
}

TEST(ProxrBoardSim, AcksBankSelectAndRelayCommands) {
  ProxrBoardSim board;
  EXPECT_EQ(board.respond(Bytes{0xFE, 0x31, 0x03}), (Bytes{0x55}));
  EXPECT_EQ(board.selected_bank(), 3);
  EXPECT_EQ(board.respond(Bytes{0xFE, 0x0A}), (Bytes{0x55}));  // on relay 2 of bank 3
  EXPECT_TRUE(board.energized(18));
  EXPECT_EQ(board.respond(Bytes{0xFE, 0x12}), (Bytes{0x01}));
  EXPECT_EQ(board.respond(Bytes{0xFE, 0x02}), (Bytes{0x55}));
  EXPECT_FALSE(board.energized(18));
  EXPECT_EQ(board.respond(Bytes{0xFE, 0x12}), (Bytes{0x00}));
}

TEST(ProxrBoardSim, UnknownOrMalformedCommandsGetNoReply) {
  ProxrBoardSim board;
  EXPECT_TRUE(board.respond(Bytes{}).empty());
  EXPECT_TRUE(board.respond(Bytes{0x00, 0x08}).empty());        // no 254 prefix
  EXPECT_TRUE(board.respond(Bytes{0xFE}).empty());              // truncated
  EXPECT_TRUE(board.respond(Bytes{0xFE, 0x31}).empty());        // bank missing
  EXPECT_TRUE(board.respond(Bytes{0xFE, 0x31, 0x00}).empty());  // bank out of range
  EXPECT_TRUE(board.respond(Bytes{0xFE, 0x31, 0x21}).empty());
  EXPECT_TRUE(board.respond(Bytes{0xFE, 0x18}).empty());        // unsupported opcode
  EXPECT_EQ(board.selected_bank(), 1);
}

TEST(ProxrBoardSim, ExternalSetDoesNotNotify) {
  int calls = 0;
  ProxrBoardSim board([&](std::int64_t, bool) { ++calls; });
  board.set_energized(7, true);
  EXPECT_TRUE(board.energized(7));
  EXPECT_EQ(calls, 0);
  board.respond(Bytes{0xFE, 0x07});
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(board.energized(7));
}

TEST(ProxrBoardSim, OutOfRangeIndexIsNeverEnergized) {
  ProxrBoardSim board;
  board.set_energized(256, true);
  board.set_energized(-1, true);
  EXPECT_FALSE(board.energized(256));
  EXPECT_FALSE(board.energized(-1));
}
