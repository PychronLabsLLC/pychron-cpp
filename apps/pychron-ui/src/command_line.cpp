#include "command_line.hpp"

namespace pychron::ui {

Result<CommandLine> parse_command_line(const QStringList& args) {
  CommandLine cli;
  for (qsizetype i = 0; i < args.size(); ++i) {
    const QString& arg = args[i];
    if (arg == QStringLiteral("--sim")) {
      cli.sim = true;
    } else if (arg == QStringLiteral("--spectrometer")) {
      if (i + 1 >= args.size() || args[i + 1].startsWith(QStringLiteral("--"))) {
        return fail(ErrorKind::Config, "--spectrometer needs a file");
      }
      cli.spectrometer_file = std::filesystem::path(args[++i].toStdString());
    } else {
      cli.files.emplace_back(arg.toStdString());
    }
  }
  return cli;
}

}  // namespace pychron::ui
