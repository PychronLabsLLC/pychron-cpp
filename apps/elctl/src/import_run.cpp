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

// Set by Ctrl-C while a run is going; read after each batch. Written from a
// signal handler, so it must not take a lock.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> g_interrupted{false};
// What handled SIGINT before the run: a second Ctrl-C is handed to it.
static_assert(std::atomic<void (*)(int)>::is_always_lock_free);
std::atomic<void (*)(int)> g_previous_handler{SIG_DFL};

std::function<void()>& batch_hook() {
  static std::function<void()> hook;
  return hook;
}

}  // namespace

void set_import_batch_hook(std::function<void()> hook) { batch_hook() = std::move(hook); }

}  // namespace elctl

// The first Ctrl-C asks the run to stop after the batch it is writing. The
// second does not wait (a fetch or a batch can take long): the handler that
// was there before the run is put back and the signal raised again, which by
// default ends the process. The batch in flight is one transaction: it is
// either stored or not.
extern "C" void elctl_import_on_interrupt(int signal_number) {
  if (!elctl::g_interrupted.exchange(true)) {
    std::signal(signal_number, elctl_import_on_interrupt);  // where the C library resets a handler once it ran
    return;
  }
  std::signal(signal_number, elctl::g_previous_handler.load());
  std::raise(signal_number);
}

#ifdef _WIN32
namespace {
// Runs on a thread of its own. TRUE: handled, the process goes on. FALSE on
// the second event: the next handler, by default the one that ends the
// process, takes it.
BOOL WINAPI elctl_import_on_console_event(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  return elctl::g_interrupted.exchange(true) ? FALSE : TRUE;
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
    g_previous_handler.store(previous_ == SIG_ERR ? SIG_DFL : previous_);
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
  // A dry run writes nothing, the importer's client row included; the
  // writer does not use the client then.
  P::Uuid client;
  if (!dry_run) {
    auto registered = importer_client(*ctx.store);
    if (!registered) return fatal(ctx.io, registered.error());
    client = *registered;
  }

  const InterruptGuard guard;
  // `unusable`: a source could not be opened. It is said, the others are
  // imported, and the command exits 2.
  bool blocking = false, paused = false, unusable = false;
  for (std::size_t i = 0; i < chosen->size(); ++i) {
    const Source& source = (*chosen)[i];
    // Stopped between two sources: the next one waits where it was.
    if (g_interrupted.load() || (limit && *limit == 0)) {
      print_paused(ctx.io, source.name, source.info.done, source.info.total);
      paused = true;
      break;
    }
    if (!source.settings) {
      report_unusable(ctx.io, missing_settings(ctx, source));
      unusable = true;
      continue;
    }
    const SourceSettings& settings = *source.settings;
    // A dry run reads the mirror as it is: fetching would write to the cache.
    if (dry_run && settings.mirror) {
      std::error_code code;
      if (!fs::is_directory(settings.path, code)) {
        report_unusable(ctx.io, source.name + ": the mirror of " + settings.url + " is not in the cache (" +
                                    utf8(settings.path) + "), and a dry run fetches nothing; run the import first");
        unusable = true;
        continue;
      }
    }
    auto opened = open_adapter(ctx, settings, *all, batch, !dry_run);
    if (!opened) {
      report_unusable(ctx.io, source.name + ": " + one_line(opened.error().what));
      unusable = true;
      continue;
    }
    for (const auto& line : opened->warnings) ctx.io.err << "warning: " << line << '\n';

    ingest::WriterConfig config = writer_config(settings);
    config.dry_run = dry_run;
    config.replay = flags.has("--replay");
    ingest::BatchWriter writer(*ctx.store, client, std::move(config));
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
        paused = true;
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
      paused = true;
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
  if (unusable) return kUsage;
  // A paused run is not a verdict on what it has imported so far.
  return blocking && !paused ? kFailed : kOk;
}

}  // namespace elctl::import_detail
