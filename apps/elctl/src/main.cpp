#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return elctl::run(args, elctl::Io{std::cin, std::cout, std::cerr});
}
