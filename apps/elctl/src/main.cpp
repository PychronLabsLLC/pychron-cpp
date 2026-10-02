#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "exp.hpp"
#include "pychron/core/log_hub.hpp"

extern "C" void elctl_on_interrupt(int) { ++elctl::interrupt_count(); }

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
  std::signal(SIGINT, elctl_on_interrupt);
  std::vector<std::string> args(argv + 1, argv + argc);
  return elctl::run(args, elctl::Io{std::cin, std::cout, std::cerr});
}
