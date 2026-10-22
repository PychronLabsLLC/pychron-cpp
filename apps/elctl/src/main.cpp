#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "exp.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/process_log.hpp"
#include "process_log.hpp"

extern "C" void elctl_on_interrupt(int) { ++elctl::interrupt_count(); }

int main(int argc, char** argv) {
  pychron::LogHub::install_crash_handlers();
  std::signal(SIGINT, elctl_on_interrupt);
  std::vector<std::string> args(argv + 1, argv + argc);
  // One hub for the process: every command logs to elctl.log. It outlives run()
  // and goes before the clock it was given.
  const pychron::SteadyClock clock;
  pychron::set_process_log_hub(
      elctl::open_process_log(args, clock, pychron::host_os(), pychron::process_env(), std::cerr));
  const int code = elctl::run(args, elctl::Io{std::cin, std::cout, std::cerr});
  pychron::set_process_log_hub(nullptr);
  return code;
}
