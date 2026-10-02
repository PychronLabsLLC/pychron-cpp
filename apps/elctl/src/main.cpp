#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pychron/core/log_hub.hpp"

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
  std::vector<std::string> args(argv + 1, argv + argc);
  return elctl::run(args, elctl::Io{std::cin, std::cout, std::cerr});
}
