#include "pychron/codecs/chromium.hpp"

#include <cmath>

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::codec::chromium;

// Command strings are the vendor reference's (Chromium Software Command
// Interface Reference, Photon Machines 2013): `<component>.<command>
// [v1,v2,...]`, LF-terminated over TCP. Replies end in CR.

TEST(Chromium, EncodesEveryCommandWithLfAndNoReplyForActions) {
  EXPECT_EQ(to_string(sys_id().tx), "Sys.ID?\n");
  EXPECT_EQ(sys_id().reply, reply_frame());
  EXPECT_EQ(to_string(laser_status().tx), "Laser.Status?\n");
  EXPECT_EQ(to_string(laser_interlocks().tx), "Laser.Interlocks?\n");
  EXPECT_EQ(to_string(laser_enabled().tx), "Laser.Enable?\n");
  EXPECT_EQ(to_string(laser_output_query().tx), "Laser.Output?\n");
  EXPECT_EQ(to_string(stage_position().tx), "Stage.Pos?\n");
  EXPECT_EQ(to_string(stage_limits().tx), "Stage.Status?\n");
  EXPECT_EQ(to_string(scans_count().tx), "Scans.Count?\n");
  EXPECT_TRUE(stage_position().expects_reply());

  EXPECT_EQ(to_string(laser_enable(true).tx), "Laser.Enable 1\n");
  EXPECT_EQ(to_string(laser_enable(false).tx), "Laser.Enable 0\n");
  EXPECT_FALSE(laser_enable(true).expects_reply());
  EXPECT_EQ(to_string(laser_fire().tx), "Laser.Fire\n");
  EXPECT_EQ(to_string(laser_stop().tx), "Laser.Stop\n");
  EXPECT_EQ(to_string(stage_stop().tx), "Stage.Stop\n");
  EXPECT_EQ(to_string(scans_stop().tx), "Scans.Stop\n");
  EXPECT_EQ(to_string(scans_status_verbosity(1).tx), "Scans.Status_Verbosity 1\n");
  EXPECT_EQ(to_string(stage_move_to({1500, -2000, 500}, {5000, 5000, 100}).tx),
            "Stage.MoveTo 1500,-2000,500,5000,5000,100\n");
  EXPECT_FALSE(stage_move_to({}, {}).expects_reply());
  EXPECT_EQ(to_string(scan_move_to(3)->tx), "Scans.MoveTo 3\n");
  EXPECT_FALSE(scan_move_to(3)->expects_reply());
  EXPECT_EQ(to_string(scan_in_position(3)->tx), "Scans.InPos? 3\n");
  EXPECT_TRUE(scan_in_position(3)->expects_reply());
  EXPECT_EQ(scan_move_to(0).error().kind, ErrorKind::Config);
  EXPECT_EQ(scan_in_position(-1).error().kind, ErrorKind::Config);
}

TEST(Chromium, OutputFormatsWithoutLocale) {
  EXPECT_EQ(to_string(laser_output(12.5)->tx), "Laser.Output 12.5\n");
  EXPECT_EQ(to_string(laser_output(100)->tx), "Laser.Output 100\n");
  EXPECT_EQ(to_string(laser_output(0)->tx), "Laser.Output 0\n");
  EXPECT_EQ(to_string(laser_output(33.333333)->tx), "Laser.Output 33.333\n");  // 3 decimals, trailing zeros cut
  EXPECT_EQ(to_string(laser_output(0.5)->tx), "Laser.Output 0.5\n");
  EXPECT_FALSE(laser_output(10)->expects_reply());
  for (double bad : {-0.1, 100.1, std::nan(""), HUGE_VAL}) {
    ASSERT_FALSE(laser_output(bad));
    EXPECT_EQ(laser_output(bad).error().kind, ErrorKind::Config);
  }
}

TEST(Chromium, DecodesRepliesFramedByCrOrCrLf) {
  EXPECT_EQ(*decode_position(to_bytes("1500,-2000,500\r")), (Microns{1500, -2000, 500}));
  // a leading LF is the tail of the previous CRLF reply; decimals are rounded
  EXPECT_EQ(*decode_position(to_bytes("\n1500.4,-2000.6,500\r")), (Microns{1500, -2001, 500}));
  EXPECT_TRUE(*decode_flag(to_bytes("1\r")));
  EXPECT_FALSE(*decode_flag(to_bytes("0\r\n")));
  EXPECT_EQ(*decode_number(to_bytes("12.5\r")), 12.5);
  EXPECT_EQ(*decode_number(to_bytes(" 0 \r")), 0.0);
  EXPECT_EQ(*decode_text(to_bytes(" Idle: Idle \r")), "Idle: Idle");
  EXPECT_TRUE(decode_interlocks(to_bytes("\r"))->empty());
  EXPECT_EQ(*decode_interlocks(to_bytes("Door, Coolant Flow\r")), (std::vector<std::string>{"Door", "Coolant Flow"}));
  const auto limits = decode_limits(to_bytes("0,-1,+1\r"));
  ASSERT_TRUE(limits);
  EXPECT_EQ(limits->x, 0);
  EXPECT_EQ(limits->y, -1);
  EXPECT_EQ(limits->z, 1);
  EXPECT_EQ(*decode_id(to_bytes("CHROMIUM 2013.1.1.0\r")), "CHROMIUM 2013.1.1.0");
  EXPECT_EQ(*decode_id(to_bytes("Chromium 2.1\r")), "Chromium 2.1");
}

TEST(Chromium, ErrorRepliesMapToKinds) {
  EXPECT_EQ(error_code(to_bytes("?4\r")), 4);
  EXPECT_EQ(error_code(to_bytes("\n?0\r")), 0);
  EXPECT_EQ(error_code(to_bytes("12.5\r")), std::nullopt);
  EXPECT_EQ(error_code(to_bytes("?\r")), std::nullopt);
  EXPECT_EQ(error_code(to_bytes("?9\r")), std::nullopt);  // not one of the five

  EXPECT_EQ(decode_number(to_bytes("?3\r")).error().kind, ErrorKind::Config);
  EXPECT_EQ(decode_number(to_bytes("?3\r")).error().code, "chromium?3");
  EXPECT_EQ(decode_flag(to_bytes("?2\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_text(to_bytes("?1\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_id(to_bytes("?0\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_position(to_bytes("?4\r")).error().kind, ErrorKind::Io);
  EXPECT_EQ(decode_position(to_bytes("?4\r")).error().code, "chromium?4");
  EXPECT_TRUE(decode_position(to_bytes("?4\r")).error().device.empty());

  const Error e = to_error(4, "Laser.Fire");
  EXPECT_EQ(e.kind, ErrorKind::Io);
  EXPECT_NE(e.what.find("Laser.Fire"), std::string::npos);
  EXPECT_NE(e.what.find("not supported by this hardware or failed"), std::string::npos);
}

TEST(Chromium, GarbageIsProtocol) {
  EXPECT_EQ(decode_position(to_bytes("1500,abc\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_position(to_bytes("1500,2000\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_flag(to_bytes("yes\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_number(to_bytes("\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_number(to_bytes("1e9999\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_id(to_bytes("NGX 1.0\r")).error().kind, ErrorKind::Protocol);
  EXPECT_NE(decode_id(to_bytes("NGX 1.0\r")).error().what.find("NGX 1.0"), std::string::npos);
  EXPECT_EQ(decode_limits(to_bytes("0,2,0\r")).error().kind, ErrorKind::Protocol);
}
