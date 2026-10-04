// elctl import run: import the registered sources, resumably. One progress
// line per batch on stderr; a summary per source on stdout.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <ostream>
#include <utility>

#include "import.hpp"
#include "import_impl.hpp"

// Last, so that its macros reach nothing but this file.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace elctl {

namespace {

// Set by Ctrl-C while a run is going; read after each batch.
std::atomic<bool> g_interrupted{false};

std::function<void()>& batch_hook() {
  static std::function<void()> hook;
  return hook;
}

}  // namespace

void set_import_batch_hook(std::function<void()> hook) { batch_hook() = std::move(hook); }

}  // namespace elctl

extern "C" void elctl_import_on_interrupt(int) { elctl::g_interrupted.store(true); }

#ifdef _WIN32
namespace {
// Runs on a thread of its own. TRUE: handled, the process goes on.
BOOL WINAPI elctl_import_on_console_event(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  elctl::g_interrupted.store(true);
  return TRUE;
}
}  // namespace
#endif

namespace elctl::import_detail {

namespace ingest = pychron::ingest;

namespace {

// Catches Ctrl-C for as long as it lives, then puts back what was there.
class InterruptGuard {
 public:
  InterruptGuard() {
    g_interrupted.store(false);
    previous_ = std::signal(SIGINT, elctl_import_on_interrupt);
#ifdef _WIN32
    SetConsoleCtrlHandler(elctl_import_on_console_event, TRUE);
#endif
  }
  ~InterruptGuard() {
#ifdef _WIN32
    SetConsoleCtrlHandler(elctl_import_on_console_event, FALSE);
#endif
    if (previous_ != SIG_ERR) std::signal(SIGINT, previous_);
  }
  InterruptGuard(const InterruptGuard&) = delete;
  InterruptGuard& operator=(const InterruptGuard&) = delete;

 private:
  void (*previous_)(int) = SIG_DFL;
};

const char* units_of(P::ImportSourceKind kind) { return kind == P::ImportSourceKind::LegacyDb ? "rows" : "commits"; }

void print_paused(Io io, const std::string& name, int done, int total) {
  io.out << "paused: " << name << " at " << done << '/' << total << '\n';
}

}  // namespace

int import_run(Context& ctx, const Flags& flags) {
  if (flags.has("--all") && flags.get("--source")) return fatal(ctx.io, "run takes --source or --all, not both");
  std::optional<int> batch, limit;
  for (const auto& [flag, into] : {std::pair{"--batch", &batch}, std::pair{"--limit", &limit}})
    if (const auto text = flags.get(flag)) {
      auto number = positive_int(flag, *text);
      if (!number) return fatal(ctx.io, number.error());
      *into = *number;
    }
  const bool dry_run = flags.has("--dry-run");

  auto all = registered_sources(ctx);
  if (!all) return fatal(ctx.io, all.error());
  auto chosen = select_sources(ctx, flags.get("--source"));
  if (!chosen) return fatal(ctx.io, chosen.error());
  if (chosen->empty()) {
    ctx.io.err << "no source is registered; see elctl import add\n";
    return kOk;
  }
  auto client = importer_client(*ctx.store);
  if (!client) return fatal(ctx.io, client.error());

  const InterruptGuard guard;
  bool blocking = false;
  for (std::size_t i = 0; i < chosen->size(); ++i) {
    const Source& source = (*chosen)[i];
    // Stopped between two sources: the next one waits where it was.
    if (g_interrupted.load() || (limit && *limit == 0)) {
      print_paused(ctx.io, source.name, source.info.done, source.info.total);
      break;
    }
    if (!source.settings)
      return fatal(ctx.io, "no settings for " + source.name + " in " + ctx.cache.string() +
                               " (" + settings_file(ctx.cache, source.info.spec.uuid).filename().string() +
                               "); was it added with another --cache?");
    const SourceSettings& settings = *source.settings;
    auto opened = open_adapter(ctx, settings, *all, batch, true);
    if (!opened) return fatal(ctx.io, opened.error());
    for (const auto& line : opened->warnings) ctx.io.err << "warning: " << line << '\n';

    ingest::WriterConfig config = writer_config(settings);
    config.dry_run = dry_run;
    config.replay = flags.has("--replay");
    ingest::BatchWriter writer(*ctx.store, *client, std::move(config));
    const char* units = units_of(settings.kind);
    int done = source.info.done, total = source.info.total;
    auto stats = writer.run(
        *opened->adapter, limit, [] { return !g_interrupted.load(); },
        [&](const ingest::RunStats& so_far, const ingest::ImportBatch& written) {
          done = written.done;
          total = written.total;
          ctx.io.err << source.name << ' ' << done << '/' << total << ' ' << units << ", " << so_far.analyses
                     << " analyses, " << so_far.conflicts << " conflicts" << std::endl;
          if (batch_hook()) batch_hook()();
        });
    if (!stats) return fatal(ctx.io, source.name + ": " + stats.error().what);
    if (limit) *limit -= std::min(*limit, stats->batches);

    const std::string counts = "analyses " + std::to_string(stats->analyses) + ", changesets " +
                               std::to_string(stats->changesets) + ", revisions " + std::to_string(stats->revisions) +
                               ", conflicts " + std::to_string(stats->conflicts);
    if (dry_run) {
      ctx.io.out << source.name << ": dry run, would write " << stats->would_write << " rows (" << counts << ")\n";
      if (!stats->finished) {
        print_paused(ctx.io, source.name, done, total);
        break;
      }
      continue;
    }
    // What the store has now: a run with nothing left to do writes no batch.
    if (auto now = ctx.store->import_sources())
      for (const auto& info : *now)
        if (info.spec.uuid == source.info.spec.uuid) {
          done = info.done;
          total = info.total;
        }
    if (!stats->finished) {
      print_paused(ctx.io, source.name, done, total);
      break;
    }
    auto pending = pending_conflicts(*ctx.store, source.info.spec.uuid);
    if (!pending) return fatal(ctx.io, pending.error());
    ctx.io.out << source.name << ": finished " << done << '/' << total << ' ' << units << '\n'
               << "  " << counts << '\n'
               << "  conflicts pending: " << describe(*pending, true) << '\n';
    if (pending->blocking.contains("unknown_analysis"))
      ctx.io.out << "  analyses were refused for an identifier the catalog does not have; once it has it,"
                    " import them with: elctl import run --replay --source "
                 << source.name << '\n';
    blocking = blocking || pending->blocking_total > 0;
  }
  return blocking ? kFailed : kOk;
}

}  // namespace elctl::import_detail
