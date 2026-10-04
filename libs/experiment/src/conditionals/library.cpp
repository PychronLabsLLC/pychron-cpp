#include "pychron/experiment/conditionals/library.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace pychron::experiment {

namespace {

Unexpected<Error> cfg(const std::string& m) { return fail(ErrorKind::Config, m); }

bool plain_name(std::string_view name) {
  return !name.empty() && name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
         name.find("..") == std::string_view::npos;
}

std::string_view strip_alias(std::string_view name) {
  constexpr std::string_view prefix = "@conditionals.";
  if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
  return name;
}

}  // namespace

Result<std::optional<std::string>> DirectoryConditionalSource::text(std::string_view name) const {
  if (!plain_name(name)) return cfg("conditionals name '" + std::string(name) + "' must be a plain file name");
  const auto path = dir_ / (std::string(name) + ".toml");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return std::optional<std::string>{};
  std::ifstream in(path);
  if (!in) return fail(ErrorKind::Io, "cannot read " + path.string());
  std::stringstream ss;
  ss << in.rdbuf();
  return std::optional<std::string>(ss.str());
}

std::string DirectoryConditionalSource::location(std::string_view name) const {
  return (dir_ / (std::string(name) + ".toml")).string();
}

Result<std::optional<std::string>> MapConditionalSource::text(std::string_view name) const {
  auto it = files_.find(name);
  if (it == files_.end()) return std::optional<std::string>{};
  return std::optional<std::string>(it->second);
}

bool ConditionalFiles::valid_name(std::string_view name) {
  return plain_name(name) && name.front() != '.' && !name.ends_with(".toml");
}

std::filesystem::path ConditionalFiles::path(std::string_view name) const {
  return dir_ / (std::string(name) + ".toml");
}

bool ConditionalFiles::exists(std::string_view name) const {
  std::error_code ec;
  return valid_name(name) && std::filesystem::is_regular_file(path(name), ec);
}

Result<std::vector<std::string>> ConditionalFiles::list() const {
  namespace fs = std::filesystem;
  std::vector<std::string> names;
  std::error_code ec;
  if (!fs::is_directory(dir_, ec)) return names;
  for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".toml") continue;
    std::string name = it->path().stem().string();
    if (valid_name(name)) names.push_back(std::move(name));
  }
  if (ec) return fail(ErrorKind::Io, "cannot list " + dir_.string() + ": " + ec.message());
  std::sort(names.begin(), names.end());
  if (auto it = std::find(names.begin(), names.end(), "system"); it != names.end())
    std::rotate(names.begin(), it, it + 1);
  return names;
}

Result<std::string> ConditionalFiles::read(std::string_view name) const {
  if (!valid_name(name)) return cfg("conditionals name '" + std::string(name) + "' must be a plain file name");
  const auto p = path(name);
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + p.string());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Result<void> ConditionalFiles::write(std::string_view name, std::string_view text) const {
  namespace fs = std::filesystem;
  if (!valid_name(name)) return cfg("conditionals name '" + std::string(name) + "' must be a plain file name");
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + dir_.string() + ": " + ec.message());
  const auto target = path(name);
  const auto tmp = dir_ / ("." + std::string(name) + ".toml.tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
      fs::remove(tmp, ec);
      return fail(ErrorKind::Io, "cannot write " + target.string());
    }
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(tmp, ec);
    return fail(ErrorKind::Io, "cannot write " + target.string() + ": " + why);
  }
  return {};
}

Result<void> ConditionalFiles::remove(std::string_view name) const {
  if (!valid_name(name)) return cfg("conditionals name '" + std::string(name) + "' must be a plain file name");
  const auto p = path(name);
  std::error_code ec;
  if (!std::filesystem::remove(p, ec) || ec)
    return fail(ErrorKind::Io, "cannot delete " + p.string() + (ec ? ": " + ec.message() : ": no such file"));
  return {};
}

Result<ConditionalSet> plan_truncations(const plan::MeasurementPlan& plan) {
  ConditionalSet set;
  for (std::size_t i = 0; i < plan.conditionals.truncations.size(); ++i) {
    const auto& t = plan.conditionals.truncations[i];
    auto expr = compile_check(t.check, std::nullopt, "");
    if (!expr) return cfg("conditionals.truncations[" + std::to_string(i) + "]: " + expr.error().what);
    Conditional c;
    c.name = "plan.truncation[" + std::to_string(i) + "]";
    c.kind = ConditionalKind::Truncation;
    c.check = t.check;
    c.expr = *expr;
    c.start = t.start;
    c.action.type = ActionSpec::Type::Truncate;
    c.level = ConditionalLevel::Plan;
    set.items.push_back(std::move(c));
  }
  return set;
}

Result<ConditionalSet> ConditionalLibrary::load(std::string_view name_in, ConditionalLevel level) const {
  const auto name = strip_alias(name_in);
  auto text = source_.text(name);
  if (!text) return fail(text.error());
  if (!*text) return cfg("conditionals '" + std::string(name) + "' not found");
  const auto where = source_.location(name);
  auto set = parse_conditionals(**text, where);
  if (!set) return fail(set.error());
  set->stamp(level, where);
  return set;
}

Result<ConditionalSet> ConditionalLibrary::system() const {
  auto text = source_.text("system");
  if (!text) return fail(text.error());
  if (!*text) return ConditionalSet{};
  return load("system", ConditionalLevel::System);
}

Result<ConditionalSet> ConditionalLibrary::queue(const QueueSpec& queue) const {
  if (queue.queue_conditionals.empty()) return ConditionalSet{};
  return load(queue.queue_conditionals, ConditionalLevel::Queue);
}

Result<ConditionalSet> ConditionalLibrary::plan(const plan::MeasurementPlan& plan, std::string_view plan_name) const {
  std::vector<ConditionalSet> parts;
  for (const auto& inc : plan.conditionals.include) {
    auto s = load(inc, ConditionalLevel::Plan);
    if (!s) return fail(s.error());
    parts.push_back(std::move(*s));
  }
  auto inline_set = plan_truncations(plan);
  if (!inline_set) return fail(inline_set.error());
  inline_set->stamp(ConditionalLevel::Plan, std::string(plan_name));
  parts.push_back(std::move(*inline_set));
  return merge_levels(parts);
}

Result<ConditionalSet> ConditionalLibrary::run(const RunSpec& run) const {
  std::vector<ConditionalSet> parts;
  for (const auto& ref : run.conditionals) {
    auto s = load(ref.name, ConditionalLevel::Run);
    if (!s) return fail(s.error());
    parts.push_back(std::move(*s));
  }
  return merge_levels(parts);
}

Result<ConditionalSet> ConditionalLibrary::for_run(const QueueSpec& queue, const RunSpec& run,
                                                   const plan::MeasurementPlan& plan) const {
  auto sys = system();
  if (!sys) return fail(sys.error());
  auto q = this->queue(queue);
  if (!q) return fail(q.error());
  auto p = this->plan(plan, run.measurement.plan.empty() ? "plan" : run.measurement.plan);
  if (!p) return fail(p.error());
  auto r = this->run(run);
  if (!r) return fail(r.error());
  return merge_levels({*sys, *q, *p, *r});
}

Result<ConditionalSet> ConditionalLibrary::for_queue(const QueueSpec& queue) const {
  auto sys = system();
  if (!sys) return fail(sys.error());
  auto q = this->queue(queue);
  if (!q) return fail(q.error());
  return merge_levels({*sys, *q});
}

std::optional<Trip> RunChecks::pre_run(const RunSpec& run, const MetricContext& ctx, const Variables& vars) {
  return engine_.check_now(ConditionalKind::PreRun, ctx, vars, 0, to_string(run.id.type));
}

Result<std::optional<PostRunOutcome>> RunChecks::post_run(const RunSpec& run, const MetricContext& ctx,
                                                         ExperimentQueue& queue, std::size_t current,
                                                         const Variables& vars, const BlankFactory& blank) {
  auto trip = engine_.check_now(ConditionalKind::PostRun, ctx, vars, 0, to_string(run.id.type));
  if (!trip) return std::optional<PostRunOutcome>{};
  PostRunOutcome out;
  out.trip = *trip;
  if (trip->action.type == ActionSpec::Type::Cancel) {
    out.cancel_queue = true;
  } else {
    auto change = apply_queue_action(queue, current, trip->action, blank);
    if (!change) return fail(change.error());
    out.change = std::move(*change);
  }
  return std::optional<PostRunOutcome>(std::move(out));
}

}  // namespace pychron::experiment
