#include "command_line.hpp"

#include <cmath>

namespace pychron::ui {

Result<CommandLine> parse_command_line(const QStringList& args) {
  CommandLine cli;
  for (qsizetype i = 0; i < args.size(); ++i) {
    const QString& arg = args[i];
    auto value = [&]() -> std::optional<QString> {
      if (i + 1 >= args.size() || args[i + 1].startsWith(QStringLiteral("--"))) return std::nullopt;
      return args[++i];
    };
    if (arg == QStringLiteral("--sim")) {
      cli.sim = true;
    } else if (arg == QStringLiteral("--spectrometer") || arg == QStringLiteral("--lab") ||
               arg == QStringLiteral("--data") || arg == QStringLiteral("--queue")) {
      auto v = value();
      if (!v) {
        return fail(ErrorKind::Config,
                    arg.toStdString() + (arg == QStringLiteral("--lab") || arg == QStringLiteral("--data")
                                             ? " needs a directory"
                                             : " needs a file"));
      }
      std::filesystem::path p(v->toStdString());
      if (arg == QStringLiteral("--spectrometer")) cli.spectrometer_file = p;
      else if (arg == QStringLiteral("--lab")) cli.lab = p;
      else if (arg == QStringLiteral("--data")) cli.data = p;
      else cli.queue = p;
    } else if (arg == QStringLiteral("--sim-speed")) {
      auto v = value();
      bool ok = false;
      const double speed = v ? v->toDouble(&ok) : 0.0;
      if (!ok || !std::isfinite(speed) || speed <= 0) {
        return fail(ErrorKind::Config, "--sim-speed needs a positive number");
      }
      cli.sim_speed = speed;
    } else {
      cli.files.emplace_back(arg.toStdString());
    }
  }
  if (cli.sim_speed > 0 && !cli.sim) return fail(ErrorKind::Config, "--sim-speed needs --sim");
  return cli;
}

}  // namespace pychron::ui
