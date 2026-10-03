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
    } else if (arg == QStringLiteral("--db")) {
      auto v = value();
      if (!v || v->isEmpty()) return fail(ErrorKind::Config, "--db needs a database url");
      cli.db = v->toStdString();
    } else if (arg == QStringLiteral("--install")) {
      auto v = value();
      if (!v || v->isEmpty()) return fail(ErrorKind::Config, "--install needs an installation name");
      cli.install = v->toStdString();
    } else if (arg == QStringLiteral("--setup")) {
      cli.setup = true;
    } else if (arg == QStringLiteral("--examples")) {
      cli.examples = true;
    } else if (arg == QStringLiteral("--version")) {
      cli.version = true;
    } else if (arg == QStringLiteral("--self-test")) {
      cli.self_test = true;
    } else if (arg == QStringLiteral("--sim-speed")) {
      auto v = value();
      bool ok = false;
      const double speed = v ? v->toDouble(&ok) : 0.0;
      if (!ok || !std::isfinite(speed) || speed <= 0) {
        return fail(ErrorKind::Config, "--sim-speed needs a positive number");
      }
      cli.sim_speed = speed;
    } else if (arg.startsWith(QStringLiteral("--"))) {
      return fail(ErrorKind::Config, "unknown option " + arg.toStdString());
    } else {
      cli.files.emplace_back(arg.toStdString());
    }
  }
  if (cli.sim_speed > 0 && !cli.sim) return fail(ErrorKind::Config, "--sim-speed needs --sim");
  const int sources = (cli.files.empty() ? 0 : 1) + (cli.install ? 1 : 0) + (cli.setup ? 1 : 0) + (cli.examples ? 1 : 0);
  if (sources > 1) return fail(ErrorKind::Config, "config files, --install, --setup and --examples exclude each other");
  return cli;
}

}  // namespace pychron::ui
