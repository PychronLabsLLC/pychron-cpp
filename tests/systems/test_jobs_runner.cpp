#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "jobs_fakes.hpp"
#include "pychron/systems/jobs/job_runner.hpp"

using namespace pychron;
using namespace pychron::jobs;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;

namespace {

JobSpec returning(int value, std::atomic<int>* calls = nullptr) {
  return JobSpec{"test", [value, calls](JobContext&) -> Result<std::any> {
                   if (calls) ++*calls;
                   return std::any(value);
                 }};
}

struct Events {
  std::vector<JobStarted> started;
  std::vector<JobProgress> progress;
  std::vector<JobFinished> finished;
  SignalBus::Subscription s1, s2, s3;

  explicit Events(SignalBus& bus) {
    s1 = bus.subscribe<JobStarted>([this](const JobStarted& e) { started.push_back(e); });
    s2 = bus.subscribe<JobProgress>([this](const JobProgress& e) { progress.push_back(e); });
    s3 = bus.subscribe<JobFinished>([this](const JobFinished& e) { finished.push_back(e); });
  }
};

}  // namespace

// ---- CancelToken / Progress -------------------------------------------------

TEST(CancelToken, HookRunsOnCancelAndOnInstallWhenAlreadyCancelled) {
  CancelToken token;
  int hits = 0;
  {
    auto guard = token.on_cancel([&] { ++hits; });
    EXPECT_FALSE(token.cancelled());
    token.cancel();
    EXPECT_TRUE(token.cancelled());
    EXPECT_EQ(hits, 1);
  }
  token.cancel();  // guard gone: hook removed
  EXPECT_EQ(hits, 1);
  auto late = token.on_cancel([&] { ++hits; });
  EXPECT_EQ(hits, 2);
}

TEST(Progress, FractionAndLast) {
  EXPECT_DOUBLE_EQ((ProgressUpdate{1, 4, "", {}}.fraction()), 0.25);
  EXPECT_DOUBLE_EQ((ProgressUpdate{3, 0, "", {}}.fraction()), 0.0);
  EXPECT_DOUBLE_EQ((ProgressUpdate{9, 4, "", {}}.fraction()), 1.0);
  std::vector<std::string> seen;
  Progress p([&](const ProgressUpdate& u) { seen.push_back(u.message); });
  p.report({1, 2, "half", {}});
  EXPECT_EQ(p.last().message, "half");
  EXPECT_EQ(seen, std::vector<std::string>{"half"});
}

TEST(JobState, Names) {
  EXPECT_EQ(to_string(JobState::Queued), "queued");
  EXPECT_EQ(to_string(JobState::Cancelled), "cancelled");
  EXPECT_FALSE(is_finished(JobState::Running));
  EXPECT_TRUE(is_finished(JobState::Failed));
}

// ---- JobRunner ----------------------------------------------------------------

TEST(JobRunner, SubmitRunsOnSchedulerWithSnapshotsAndEvents) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  Events ev(r.bus);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  auto id = runner.submit(returning(42));
  ASSERT_TRUE(id.has_value());
  EXPECT_TRUE(runner.busy());
  EXPECT_EQ(runner.job(*id)->state, JobState::Queued);
  r.scheduler.run_pending();
  EXPECT_FALSE(runner.busy());

  auto job = runner.job(*id);
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state, JobState::Succeeded);
  EXPECT_EQ(job->kind, "test");
  EXPECT_EQ(std::any_cast<int>(job->result), 42);
  ASSERT_TRUE(job->before.has_value());
  ASSERT_TRUE(job->after.has_value());
  EXPECT_EQ(job->before->name, "sim-integrated");
  EXPECT_FALSE(job->error.has_value());

  ASSERT_EQ(ev.started.size(), 1U);
  EXPECT_EQ(ev.started[0].id, *id);
  EXPECT_EQ(ev.started[0].before.hash, job->before->hash);
  ASSERT_EQ(ev.finished.size(), 1U);
  EXPECT_EQ(ev.finished[0].job.id, *id);
  EXPECT_EQ(ev.finished[0].job.state, JobState::Succeeded);
}

TEST(JobRunner, SnapshotsCaptureStateBeforeAndAfter) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  auto job = runner.run(JobSpec{"hv", [](JobContext& ctx) -> Result<std::any> {
                                  if (auto s = ctx.spectrometer.set_hv(4000.0); !s) return fail(s.error());
                                  return std::any{};
                                }});
  ASSERT_TRUE(job.has_value()) << to_string(job.error());
  EXPECT_EQ(job->state, JobState::Succeeded);
  EXPECT_EQ(job->before->hv, 4500.0);
  EXPECT_EQ(job->after->hv, 4000.0);
  EXPECT_NE(job->before->hash, job->after->hash);
}

TEST(JobRunner, SecondSubmitWhileQueuedIsInterlocked) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  std::atomic<int> calls = 0;
  ASSERT_TRUE(runner.submit(returning(1, &calls)).has_value());
  auto second = runner.submit(returning(2, &calls));
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(second.error().what, "spectrometer busy");
  auto sync = runner.run(returning(3, &calls));
  ASSERT_FALSE(sync.has_value());
  EXPECT_EQ(sync.error().kind, ErrorKind::Interlock);
  r.scheduler.run_pending();
  EXPECT_EQ(calls, 1);
  // Free again once the first job finished.
  EXPECT_TRUE(runner.submit(returning(4, &calls)).has_value());
}

TEST(JobRunner, SubmitFromRunningJobIsInterlocked) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  std::optional<Error> nested;
  auto job = runner.run(JobSpec{"outer", [&](JobContext&) -> Result<std::any> {
                                  auto s = runner.submit(returning(1));
                                  if (!s) nested = s.error();
                                  return std::any{};
                                }});
  ASSERT_TRUE(job.has_value());
  ASSERT_TRUE(nested.has_value());
  EXPECT_EQ(nested->kind, ErrorKind::Interlock);
}

TEST(JobRunner, EmptyBodyIsConfigError) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  auto id = runner.submit(JobSpec{"empty", {}});
  ASSERT_FALSE(id.has_value());
  EXPECT_EQ(id.error().kind, ErrorKind::Config);
  EXPECT_FALSE(runner.busy());
}

TEST(JobRunner, FailingBodyFinishesFailedWithError) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  Events ev(r.bus);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  auto job = runner.run(JobSpec{"bad", [](JobContext&) -> Result<std::any> {
                                  return fail(ErrorKind::Io, "boom", "magnet");
                                }});
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state, JobState::Failed);
  ASSERT_TRUE(job->error.has_value());
  EXPECT_EQ(job->error->what, "boom");
  EXPECT_TRUE(job->after.has_value());
  ASSERT_EQ(ev.finished.size(), 1U);
  EXPECT_EQ(ev.finished[0].job.state, JobState::Failed);
}

TEST(JobRunner, CancelQueuedJobNeverRunsIt) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  Events ev(r.bus);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  std::atomic<int> calls = 0;
  auto id = runner.submit(returning(1, &calls));
  ASSERT_TRUE(id.has_value());
  ASSERT_TRUE(runner.cancel(*id).has_value());
  r.scheduler.run_pending();
  EXPECT_EQ(calls, 0);
  auto job = runner.job(*id);
  EXPECT_EQ(job->state, JobState::Cancelled);
  EXPECT_FALSE(job->before.has_value());
  EXPECT_FALSE(runner.busy());
  EXPECT_TRUE(ev.started.empty());
  ASSERT_EQ(ev.finished.size(), 1U);
  // Finished jobs cannot be cancelled again.
  auto again = runner.cancel(*id);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().kind, ErrorKind::Config);
}

TEST(JobRunner, CancelRunningJobSignalsToken) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  std::atomic<bool> running = false;
  auto id = runner.submit(JobSpec{"spin", [&](JobContext& ctx) -> Result<std::any> {
                                    running = true;
                                    while (!ctx.cancel.cancelled()) std::this_thread::sleep_for(1ms);
                                    return fail(ErrorKind::Cancelled, "stopped");
                                  }});
  ASSERT_TRUE(id.has_value());
  auto worker = std::async(std::launch::async, [&] { r.scheduler.run_pending(); });
  while (!running) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(runner.job(*id)->state, JobState::Running);
  EXPECT_EQ(runner.current(), std::optional<JobId>(*id));
  ASSERT_TRUE(runner.cancel(*id).has_value());
  worker.get();
  runner.wait_idle();
  EXPECT_EQ(runner.job(*id)->state, JobState::Cancelled);
  EXPECT_TRUE(runner.job(*id)->after.has_value());
}

TEST(JobRunner, CancelUnknownJobIsConfigError) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  auto c = runner.cancel(99);
  ASSERT_FALSE(c.has_value());
  EXPECT_EQ(c.error().kind, ErrorKind::Config);
}

TEST(JobRunner, ProgressIsPublishedAndKept) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  Events ev(r.bus);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  auto job = runner.run(JobSpec{"steps", [](JobContext& ctx) -> Result<std::any> {
                                  ctx.progress.report({1, 2, "one", {}});
                                  ctx.progress.report({2, 2, "two", {}});
                                  return std::any{};
                                }});
  ASSERT_TRUE(job.has_value());
  ASSERT_EQ(ev.progress.size(), 2U);
  EXPECT_EQ(ev.progress[0].id, job->id);
  EXPECT_EQ(ev.progress[0].kind, "steps");
  EXPECT_EQ(ev.progress[1].update.message, "two");
  EXPECT_EQ(job->progress.message, "two");
}

TEST(JobRunner, IdsIncreaseAndHistoryIsBounded) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock, JobRunner::Options{2});
  std::vector<JobId> ids;
  for (int i = 0; i < 3; ++i) {
    auto job = runner.run(returning(i));
    ASSERT_TRUE(job.has_value());
    ids.push_back(job->id);
  }
  EXPECT_LT(ids[0], ids[1]);
  EXPECT_LT(ids[1], ids[2]);
  EXPECT_FALSE(runner.job(ids[0]).has_value());
  EXPECT_TRUE(runner.job(ids[2]).has_value());
}

TEST(JobRunner, SweepJobEndToEnd) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [&](const spectrometer::ChannelId&) { return r.positioner.value * 10.0; };
  Events ev(r.bus);
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  SweepSpec s;
  s.start = 1.0;
  s.stop = 2.0;
  s.step = 0.5;
  s.record = {"H1"};
  auto fut = std::async(std::launch::async, [&] {
    return runner.run(sweep_job(s, Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never}));
  });
  auto job = r.drive(fut);
  ASSERT_TRUE(job.has_value()) << to_string(job.error());
  EXPECT_EQ(job->state, JobState::Succeeded);
  EXPECT_EQ(job->kind, "sweep");
  const auto& points = std::any_cast<const std::vector<SweepPoint>&>(job->result);
  ASSERT_EQ(points.size(), 3U);
  EXPECT_NEAR(points[2].y.at("H1"), 20.0, 1e-9);
  EXPECT_EQ(job->after->magnet, 2.0);
  std::size_t with_points = 0;
  for (const auto& p : ev.progress) with_points += p.update.point ? 1 : 0;
  EXPECT_EQ(with_points, 3U);
}

TEST(JobRunner, CancelRunningSweepJob) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const spectrometer::ChannelId&) { return 1.0; };
  JobRunner runner(*r.spec, r.scheduler, r.bus, r.clock);
  SweepSpec s;
  s.start = 0.0;
  s.stop = 5.0;
  s.step = 1.0;
  auto id = runner.submit(sweep_job(s, Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never}));
  ASSERT_TRUE(id.has_value());
  // Engine never polled: the sweep blocks in its first acquisition.
  auto worker = std::async(std::launch::async, [&] { r.scheduler.run_pending(); });
  while (runner.job(*id)->state != JobState::Running) std::this_thread::sleep_for(1ms);
  while (worker.wait_for(5ms) != std::future_status::ready) (void)runner.cancel(*id);
  runner.wait_idle();
  auto job = runner.job(*id);
  EXPECT_EQ(job->state, JobState::Cancelled);
  ASSERT_TRUE(job->error.has_value());
  EXPECT_EQ(job->error->kind, ErrorKind::Cancelled);
}
