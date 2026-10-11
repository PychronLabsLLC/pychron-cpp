#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "pychron/core/host_name.hpp"

// The machine's name comes from the machine, not from HOSTNAME, which a
// shell sets for itself and rarely exports: with it every program was
// "localhost" to the database.
TEST(HostName, IsTheMachinesAndNeverEmpty) {
  const std::string name = pychron::host_name();
  EXPECT_FALSE(name.empty());
#ifndef _WIN32
  std::array<char, 256> buf{};
  ASSERT_EQ(::gethostname(buf.data(), buf.size() - 1), 0);
  if (buf[0] != '\0') EXPECT_EQ(name, std::string(buf.data()));
  // HOSTNAME has no say.
  ::setenv("HOSTNAME", "not-this-machine", 1);
  EXPECT_EQ(pychron::host_name(), name);
  ::unsetenv("HOSTNAME");
  EXPECT_EQ(pychron::host_name(), name);
#endif
}
