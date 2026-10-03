#include "pychron/dvc/git_reader.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <new>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "pychron/core/process.hpp"
#include "pychron/core/sha256.hpp"

namespace pychron::dvc {
namespace {

constexpr char kFieldSeparator = '\x1f';

// ------------------------------------------------------------ small helpers

bool is_sha(std::string_view text) {
  if (text.size() != 40 && text.size() != 64) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
  while (!text.empty() && (text.front() == '\n' || text.front() == ' ')) text.remove_prefix(1);
  return text;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = text.find(separator, start);
    if (end == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
}

// One sha per line.
std::string lines_of(std::span<const std::string> shas) {
  std::string input;
  for (const auto& sha : shas) {
    input += sha;
    input += '\n';
  }
  return input;
}

std::optional<int> two_digits(std::string_view text) {
  if (text.size() != 2 || text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return std::nullopt;
  return (text[0] - '0') * 10 + (text[1] - '0');
}

// git's %aI: "2016-03-04T05:06:07-07:00", or with "Z" for +00:00.
std::optional<persistence::UtcTime> parse_author_date(std::string_view iso) {
  if (iso.size() < 20) return std::nullopt;
  auto time = persistence::UtcTime::parse(std::string(iso.substr(0, 19)) + "Z");
  if (!time) return std::nullopt;
  const std::string_view zone = iso.substr(19);
  if (zone == "Z") return time;
  if (zone.size() != 6 || (zone[0] != '+' && zone[0] != '-') || zone[3] != ':') return std::nullopt;
  const auto hours = two_digits(zone.substr(1, 2));
  const auto minutes = two_digits(zone.substr(4, 2));
  if (!hours || !minutes) return std::nullopt;
  const std::int64_t offset_micros = (std::int64_t{*hours} * 60 + *minutes) * 60 * 1'000'000;
  time->micros += zone[0] == '+' ? -offset_micros : offset_micros;  // local = UTC + offset
  return time;
}

// ---------------------------------------------------------------- scratch

// A uniquely named file for one command's stdout, removed on every path out.
class ScratchFile {
 public:
  explicit ScratchFile(const std::filesystem::path& directory) {
    static const std::string token = [] {
      std::random_device device;
      std::ostringstream text;
      text << std::hex << device() << device();
      return text.str();
    }();
    static std::atomic<std::uint64_t> counter{0};
    path_ = directory / ("git-reader-" + token + "-" + std::to_string(counter.fetch_add(1)) + ".out");
  }
  ~ScratchFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  ScratchFile(const ScratchFile&) = delete;
  ScratchFile& operator=(const ScratchFile&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// No exception leaves the library: a buffer too large to allocate is false.
bool resize_to(std::string& text, std::uintmax_t size) noexcept {
  try {
    if (size > text.max_size()) return false;
    text.resize(static_cast<std::size_t>(size));
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

Result<std::string> read_whole(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + path.string());
  std::string text;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size > 0) {
    if (!resize_to(text, static_cast<std::uintmax_t>(size)))
      return fail(ErrorKind::Io, "out of memory reading " + std::to_string(size) + " bytes of " + path.string());
    in.seekg(0, std::ios::beg);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (static_cast<std::size_t>(in.gcount()) != text.size()) return fail(ErrorKind::Io, "short read of " + path.string());
  }
  return text;
}

// ------------------------------------------------------------- running git

#ifdef _WIN32
constexpr const char* kNullDevice = "NUL";
#else
constexpr const char* kNullDevice = "/dev/null";
#endif

// What every child gets: no system or global configuration, no prompt, and
// the repository named outright. Each of the repository variables is set, so
// one inherited from the parent process is replaced rather than obeyed, and
// git has nothing left to discover. `work_tree` is not read by any command
// here; it is set only so that an inherited value is not used.
std::vector<std::pair<std::string, std::string>> environment(const std::filesystem::path& git_dir,
                                                             const std::filesystem::path& common_dir,
                                                             const std::filesystem::path& work_tree) {
  return {{"GIT_CONFIG_NOSYSTEM", "1"},
          {"GIT_CONFIG_GLOBAL", kNullDevice},
          {"GIT_TERMINAL_PROMPT", "0"},
          {"LC_ALL", "C"},
          {"GIT_DIR", git_dir.string()},
          {"GIT_COMMON_DIR", common_dir.string()},
          {"GIT_OBJECT_DIRECTORY", (common_dir / "objects").string()},
          {"GIT_ALTERNATE_OBJECT_DIRECTORIES", ""},
          {"GIT_INDEX_FILE", (git_dir / "index").string()},
          {"GIT_WORK_TREE", work_tree.string()}};
}

std::string first_line(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::string line;
  if (in) std::getline(in, line, '\n');
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  return line;
}

// Where a repository keeps its files, read from the directory itself and not
// asked of git (whose answer an inherited GIT_DIR would change):
//   <repo>/.git a directory   an ordinary work tree
//   <repo>/.git a file        a linked work tree or submodule: "gitdir: <path>"
//   otherwise                 <repo> is itself the git directory (bare, mirror)
// A linked work tree's git directory names the shared one in `commondir`.
// Whether any of it is a repository is for git to say.
struct Layout {
  std::filesystem::path git_dir, common_dir;
};

Layout layout_of(const std::filesystem::path& repo) {
  std::error_code code;
  std::filesystem::path root = std::filesystem::absolute(repo, code);
  if (code) root = repo;
  Layout layout{root, {}};
  const std::filesystem::path dot_git = root / ".git";
  if (std::filesystem::is_directory(dot_git, code)) {
    layout.git_dir = dot_git;
  } else if (std::filesystem::is_regular_file(dot_git, code)) {
    constexpr std::string_view kPrefix = "gitdir: ";
    const std::string line = first_line(dot_git);
    if (line.starts_with(kPrefix)) {
      const std::filesystem::path target(line.substr(kPrefix.size()));
      layout.git_dir = target.is_absolute() ? target : root / target;
    }
  }
  layout.common_dir = layout.git_dir;
  if (const std::string line = first_line(layout.git_dir / "commondir"); !line.empty()) {
    const std::filesystem::path target(line);
    layout.common_dir = target.is_absolute() ? target : layout.git_dir / target;
  }
  return layout;
}

// Where the commands of one reader run.
struct Site {
  std::filesystem::path repo, git_dir, common_dir, scratch;
  std::chrono::milliseconds timeout;
};

std::string describe(const Site& site) { return "git repository " + site.repo.string(); }

ProcessSpec spec_for(const Site& site, std::vector<std::string> args, std::string input) {
  ProcessSpec spec;
  spec.argv = {"git", "--no-replace-objects", "--no-optional-locks", "-c", "core.quotepath=false",
               "-c",  "i18n.logOutputEncoding=UTF-8"};
  for (auto& arg : args) spec.argv.push_back(std::move(arg));
  spec.input = std::move(input);
  const bool bare = site.git_dir.filename() != ".git" && site.git_dir == site.common_dir;
  spec.env = environment(site.git_dir, site.common_dir, bare ? site.git_dir : site.repo);
  spec.timeout = site.timeout;
  return spec;
}

struct Ran {
  int exit_code = 0;
  std::string out;  // stdout, whole
  std::string err;  // stderr, trimmed
};

// Runs git with stdout in a scratch file. An error only when git could not
// be run or its output could not be read; the exit code is the caller's.
Result<Ran> run(const Site& site, std::vector<std::string> args, std::string input = {}) {
  const ScratchFile file(site.scratch);
  const std::string command = args.empty() ? std::string() : args.front();
  ProcessSpec spec = spec_for(site, std::move(args), std::move(input));
  spec.stdout_file = file.path();
  auto result = run_process(spec);
  if (!result)
    return fail(result.error().kind, describe(site) + ": cannot run git " + command + ": " + result.error().what);
  auto out = read_whole(file.path());
  if (!out) return fail(out.error());
  return Ran{result->exit_code, std::move(*out), std::string(trimmed(result->output))};
}

Unexpected<Error> git_failed(const Site& site, std::string_view command, const Ran& ran) {
  return fail(ErrorKind::Io, describe(site) + ": git " + std::string(command) + " failed (exit " +
                                 std::to_string(ran.exit_code) + "): " + ran.err);
}

// stdout of a command that must succeed.
Result<std::string> output(const Site& site, std::vector<std::string> args, std::string input = {}) {
  const std::string command = args.front();
  auto ran = run(site, std::move(args), std::move(input));
  if (!ran) return fail(ran.error());
  if (ran->exit_code != 0) return git_failed(site, command, *ran);
  return std::move(ran->out);
}

// The directory name of a url's mirror: its last component, made safe, and a
// hash of the whole url.
std::string mirror_name(std::string_view url) {
  std::string_view tail = url;
  while (!tail.empty() && (tail.back() == '/' || tail.back() == '\\')) tail.remove_suffix(1);
  if (tail.ends_with(".git")) tail.remove_suffix(4);
  const std::size_t cut = tail.find_last_of("/\\:");
  if (cut != std::string_view::npos) tail.remove_prefix(cut + 1);
  std::string stem;
  for (const char c : tail.substr(0, 64)) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                      (!stem.empty() && (c == '-' || c == '.'));
    stem += safe ? c : '_';
  }
  if (stem.empty()) stem = "repo";
  const Sha256Digest digest = sha256(url);
  return stem + "-" + to_hex(std::span<const std::uint8_t>(digest)).substr(0, 16) + ".git";
}

// The --raw -z output of diff-tree: NUL-separated tokens, a commit sha (when
// diff-tree read commits from stdin), then for each change ":<old mode> <new
// mode> <old sha> <new sha> <status>" and the path. `commit` names the changes
// until a sha token replaces it.
Result<std::vector<GitChange>> parse_changes(const Site& site, std::string_view out, std::string commit) {
  std::vector<GitChange> result;
  // A path may itself begin with ':', but only ever follows a change line.
  const std::vector<std::string_view> tokens = split(out, '\0');
  const auto bad = [&](std::string_view token) {
    return fail(ErrorKind::Protocol, describe(site) + ": unexpected diff-tree output: " + std::string(token.substr(0, 200)));
  };
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const std::string_view token = tokens[i];
    if (token.empty()) continue;
    if (token.front() != ':') {
      if (!is_sha(token)) return bad(token);
      commit = std::string(token);
      continue;
    }
    const std::vector<std::string_view> fields = split(token.substr(1), ' ');
    if (fields.size() != 5 || fields[4].empty() || commit.empty() || i + 1 >= tokens.size()) return bad(token);
    const std::string_view path = tokens[++i];
    const char letter = fields[4].front();
    constexpr std::string_view kSubmodule = "160000";
    GitChange change;
    change.commit = commit;
    change.path = std::string(path);
    switch (letter) {
      case 'A':
      case 'M':
      case 'T':  // the type changed (file to symlink, ...): new content at the same path
        if (fields[1] == kSubmodule) continue;
        change.status = letter == 'A' ? 'A' : 'M';
        change.blob_sha = std::string(fields[3]);
        if (!is_sha(change.blob_sha)) return bad(token);
        break;
      case 'D':
        if (fields[0] == kSubmodule) continue;
        change.status = 'D';
        break;
      default:
        return bad(token);
    }
    result.push_back(std::move(change));
  }
  return result;
}


}  // namespace

std::optional<GitVersion> parse_git_version(std::string_view text) {
  constexpr std::string_view kPrefix = "git version ";
  text = trimmed(text);
  if (!text.starts_with(kPrefix)) return std::nullopt;
  text.remove_prefix(kPrefix.size());
  // Up to three numbers separated by dots; whatever follows is the vendor's.
  int numbers[3] = {0, 0, 0};
  int count = 0;
  while (count < 3) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), numbers[count]);
    if (parsed.ec != std::errc{} || numbers[count] < 0) break;
    ++count;
    text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
    if (text.size() < 2 || text.front() != '.' || text[1] < '0' || text[1] > '9') break;
    text.remove_prefix(1);
  }
  if (count < 2) return std::nullopt;
  return GitVersion{numbers[0], numbers[1], numbers[2]};
}

// ------------------------------------------------------------------- open

GitReader::GitReader(GitConfig config, std::filesystem::path git_dir, std::filesystem::path common_dir,
                     std::string head_sha)
    : config_(std::move(config)),
      git_dir_(std::move(git_dir)),
      common_dir_(std::move(common_dir)),
      head_(std::move(head_sha)) {}

Result<GitReader> GitReader::open(GitConfig config) {
  const std::string where = "git repository " + config.repo.string();
  if (config.branch.empty()) return fail(ErrorKind::Config, where + ": no branch given");

  std::error_code code;
  if (config.scratch.empty()) config.scratch = std::filesystem::temp_directory_path(code);
  if (!code) std::filesystem::create_directories(config.scratch, code);
  if (code) {
    return fail(ErrorKind::Io,
                where + ": cannot create scratch directory " + config.scratch.string() + ": " + code.message());
  }
  Layout layout = layout_of(config.repo);
  const Site site{config.repo, layout.git_dir, layout.common_dir, config.scratch, config.timeout};

  {
    ProcessSpec spec;
    spec.argv = {"git", "--version"};
    spec.env = environment(layout.git_dir, layout.common_dir, layout.git_dir);
    spec.timeout = config.timeout;
    const auto ran = run_process(spec);
    if (!ran) {
      if (ran.error().kind == ErrorKind::Timeout) return fail(ran.error());
      return fail(ErrorKind::Io, where + ": git not found or cannot be started: " + ran.error().what);
    }
    const auto version = parse_git_version(ran->output);
    if (ran->exit_code != 0 || !version) {
      return fail(ErrorKind::Io, where + ": cannot read the git version from `git --version`: " +
                                     std::string(trimmed(ran->output)).substr(0, 200));
    }
    if (*version < kMinimumGitVersion) {
      const auto text = [](const GitVersion& v) {
        return std::to_string(v.major_number) + "." + std::to_string(v.minor_number) + "." +
               std::to_string(v.patch_number);
      };
      return fail(ErrorKind::Config, where + ": git " + text(*version) + " found; git " + text(kMinimumGitVersion) +
                                         " or newer is required");
    }
  }

  auto git_dir = run(site, {"rev-parse", "--git-dir"});
  if (!git_dir) return fail(git_dir.error());
  if (git_dir->exit_code != 0) return fail(ErrorKind::Config, where + ": not a git repository: " + git_dir->err);

  auto shallow = output(site, {"rev-parse", "--is-shallow-repository"});
  if (!shallow) return fail(shallow.error());
  if (trimmed(*shallow) != "false") {
    return fail(ErrorKind::Config,
                where + ": shallow clone; the full history is needed (git fetch --unshallow)");
  }

  // refs/heads first: a bare name would prefer a tag called the same.
  for (const std::string& name : {"refs/heads/" + config.branch, config.branch}) {
    auto resolved = run(site, {"rev-parse", "--verify", "--quiet", "--end-of-options", name + "^{commit}"});
    if (!resolved) return fail(resolved.error());
    const std::string_view sha = trimmed(resolved->out);
    if (resolved->exit_code == 0 && is_sha(sha)) {
      return GitReader(std::move(config), std::move(layout.git_dir), std::move(layout.common_dir), std::string(sha));
    }
  }

  auto any = output(site, {"rev-list", "--max-count=1", "--all"});
  if (!any) return fail(any.error());
  if (trimmed(*any).empty()) return fail(ErrorKind::Config, where + ": no commits (empty repository)");
  return fail(ErrorKind::Config, where + ": branch '" + config.branch + "' not found");
}

// ----------------------------------------------------------------- mirror

Result<std::filesystem::path> GitReader::mirror(std::string_view url, const std::filesystem::path& cache_dir,
                                                std::chrono::milliseconds timeout) {
  if (url.empty() || url.front() == '-') return fail(ErrorKind::Config, "git mirror: not a url: '" + std::string(url) + "'");
  std::error_code code;
  std::filesystem::create_directories(cache_dir, code);
  if (code) return fail(ErrorKind::Io, "git mirror: cannot create " + cache_dir.string() + ": " + code.message());
  std::filesystem::path directory = std::filesystem::absolute(cache_dir, code);
  if (code) return fail(ErrorKind::Io, "git mirror: cannot resolve " + cache_dir.string() + ": " + code.message());
  directory /= mirror_name(url);
  const bool present = std::filesystem::exists(directory, code);

  // The mirror is bare: its directory is the git directory, named outright
  // for the clone as well, which would otherwise obey an inherited GIT_DIR.
  ProcessSpec spec;
  if (present)
    spec.argv = {"git", "fetch", "--prune", "--quiet", "origin"};
  else
    spec.argv = {"git", "clone", "--mirror", "--quiet", "--", std::string(url), directory.string()};
  spec.env = environment(directory, directory, directory);
  spec.timeout = timeout;
  const auto result = run_process(spec);
  const std::string what = std::string("git mirror of ") + std::string(url) + " in " + directory.string();
  if (!result) return fail(result.error().kind, what + ": cannot run git: " + result.error().what);
  if (result->exit_code != 0) {
    return fail(ErrorKind::Io, what + ": git " + (present ? "fetch" : "clone") + " failed (exit " +
                                   std::to_string(result->exit_code) + "): " + std::string(trimmed(result->output)));
  }
  return directory;
}

// ---------------------------------------------------------------- history

Result<std::string> GitReader::default_branch() const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  auto ran = run(site, {"symbolic-ref", "--quiet", "--short", "HEAD"});
  if (!ran) return fail(ran.error());
  const std::string_view name = trimmed(ran->out);
  if (ran->exit_code != 0 || name.empty())
    return fail(ErrorKind::Config, describe(site) + ": HEAD is detached; there is no default branch");
  return std::string(name);
}

Result<bool> GitReader::is_ancestor(std::string_view ancestor, std::string_view descendant) const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  for (const std::string_view name : {ancestor, descendant}) {
    if (name.empty() || name.front() == '-')
      return fail(ErrorKind::Config, describe(site) + ": not a commit: '" + std::string(name) + "'");
  }
  auto ran = run(site, {"merge-base", "--is-ancestor", std::string(ancestor), std::string(descendant)});
  if (!ran) return fail(ran.error());
  if (ran->exit_code == 0) return true;
  if (ran->exit_code == 1) return false;
  return git_failed(site, "merge-base", *ran);
}

Result<std::vector<std::string>> GitReader::rev_list(std::optional<std::string> after) const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  std::vector<std::string> args{"rev-list", "--topo-order", "--reverse", head_};
  if (after) {
    if (!is_sha(*after)) return fail(ErrorKind::Config, describe(site) + ": not a commit sha: '" + *after + "'");
    const auto reachable = is_ancestor(*after, head_);
    if (!reachable) return fail(reachable.error());
    if (!*reachable) {
      return fail(ErrorKind::Protocol, describe(site) + ": commit " + *after + " is not an ancestor of " +
                                           config_.branch + " (" + head_ + "); history was rewritten");
    }
    args.push_back("^" + *after);
  }
  auto out = output(site, std::move(args));
  if (!out) return fail(out.error());
  std::vector<std::string> shas;
  for (const std::string_view line : split(*out, '\n')) {
    if (line.empty()) continue;
    if (!is_sha(line)) return fail(ErrorKind::Protocol, describe(site) + ": unexpected rev-list output: " + std::string(line));
    shas.emplace_back(line);
  }
  return shas;
}

Result<std::vector<GitCommit>> GitReader::commits(std::span<const std::string> shas) const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  std::vector<GitCommit> result;
  if (shas.empty()) return result;  // git log without revisions would show HEAD
  for (const auto& sha : shas)
    if (!is_sha(sha)) return fail(ErrorKind::Config, describe(site) + ": not a commit sha: '" + sha + "'");

  auto out = output(site,
                    {"log", "--no-walk=unsorted", "--stdin", "-z", "--no-show-signature",
                     "--format=%H%x00%P%x00%aI%x00%an%x00%ae%x00%B"},
                    lines_of(shas));
  if (!out) return fail(out.error());

  // Six NUL-terminated fields per commit (-z ends the last one). NUL is the
  // one byte that a name, an address or a message cannot hold.
  std::vector<std::string_view> tokens = split(*out, '\0');
  if (!tokens.empty() && tokens.back().empty()) tokens.pop_back();
  if (tokens.size() % 6 != 0) return fail(ErrorKind::Protocol, describe(site) + ": unexpected log output");
  std::unordered_map<std::string, GitCommit> by_sha;
  for (std::size_t i = 0; i < tokens.size(); i += 6) {
    const std::string_view* fields = &tokens[i];  // sha, parents, date, name, email, message
    if (!is_sha(fields[0])) return fail(ErrorKind::Protocol, describe(site) + ": unexpected log output");
    GitCommit commit;
    commit.sha = std::string(fields[0]);
    for (const std::string_view parent : split(fields[1], ' '))
      if (!parent.empty()) commit.parents.emplace_back(parent);
    commit.author.name = std::string(fields[3]);
    commit.author.email = std::string(fields[4]);
    const auto utc = parse_author_date(fields[2]);
    if (!utc) {
      return fail(ErrorKind::Protocol, describe(site) + ": commit " + commit.sha + " has an author date that cannot be read: '" +
                                           std::string(fields[2]) + "'");
    }
    commit.author.utc = *utc;
    std::string_view rest = fields[5];
    if (rest.ends_with('\n')) rest.remove_suffix(1);
    commit.message = std::string(rest);
    by_sha.insert_or_assign(commit.sha, std::move(commit));
  }

  result.reserve(shas.size());
  for (const auto& sha : shas) {
    const auto found = by_sha.find(sha);
    if (found == by_sha.end()) return fail(ErrorKind::Protocol, describe(site) + ": git log did not show commit " + sha);
    result.push_back(found->second);
  }
  return result;
}

Result<std::vector<GitChange>> GitReader::changes(std::span<const std::string> shas) const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  if (shas.empty()) return std::vector<GitChange>{};
  for (const auto& sha : shas)
    if (!is_sha(sha)) return fail(ErrorKind::Config, describe(site) + ": not a commit sha: '" + sha + "'");

  // --diff-merges=first-parent, not `-m --first-parent`: diff-tree ignores
  // --first-parent and would print a merge once per parent.
  auto out = output(site,
                    {"diff-tree", "--stdin", "-r", "-z", "--root", "--diff-merges=first-parent", "--no-renames",
                     "--raw", "--no-abbrev"},
                    lines_of(shas));
  if (!out) return fail(out.error());

  return parse_changes(site, *out, {});
}

Result<std::vector<GitChange>> GitReader::diff(const std::string& from, const std::string& to) const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  for (const auto& sha : {from, to})
    if (!is_sha(sha)) return fail(ErrorKind::Config, describe(site) + ": not a commit sha: '" + sha + "'");
  auto out = output(site, {"diff-tree", "-r", "-z", "--no-renames", "--raw", "--no-abbrev", from, to});
  if (!out) return fail(out.error());
  return parse_changes(site, *out, to);
}

Result<std::vector<GitTag>> GitReader::tags() const {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  auto out = output(site, {"for-each-ref", "--sort=refname",
                           "--format=%(refname:strip=2)%1f%(objectname)%1f%(objecttype)%1f%(*objectname)%1f%(*objecttype)",
                           "refs/tags"});
  if (!out) return fail(out.error());
  std::vector<GitTag> result;
  for (const std::string_view line : split(*out, '\n')) {
    if (line.empty()) continue;
    const std::vector<std::string_view> fields = split(line, kFieldSeparator);
    if (fields.size() != 5)
      return fail(ErrorKind::Protocol, describe(site) + ": unexpected for-each-ref output: " + std::string(line));
    if (fields[2] == "commit" && is_sha(fields[1])) {
      result.push_back(GitTag{std::string(fields[0]), std::string(fields[1])});
    } else if (fields[2] == "tag" && fields[4] == "commit" && is_sha(fields[3])) {
      result.push_back(GitTag{std::string(fields[0]), std::string(fields[3])});
    }  // a tag of a tree or a blob names no commit
  }
  return result;
}

// ------------------------------------------------------------------ blobs

Result<void> GitReader::fetch_blobs(std::span<const std::string> blob_shas) {
  const Site site{config_.repo, git_dir_, common_dir_, config_.scratch, config_.timeout};
  std::unordered_set<std::string_view> asked;
  std::vector<std::string> wanted;
  for (const auto& sha : blob_shas) {
    if (!is_sha(sha)) return fail(ErrorKind::Config, describe(site) + ": not a blob sha: '" + sha + "'");
    if (!asked.insert(sha).second) continue;
    const auto cached = index_.find(sha);
    if (cached == index_.end())
      wanted.push_back(sha);
    else
      lru_.splice(lru_.begin(), lru_, cached->second);
  }

  std::vector<Blob> fetched;
  if (!wanted.empty()) {
    const ScratchFile file(site.scratch);
    ProcessSpec spec = spec_for(site, {"cat-file", "--batch"}, lines_of(wanted));
    spec.stdout_file = file.path();
    const auto ran = run_process(spec);
    if (!ran) return fail(ran.error().kind, describe(site) + ": cannot run git cat-file: " + ran.error().what);
    if (ran->exit_code != 0) {
      return fail(ErrorKind::Io, describe(site) + ": git cat-file failed (exit " + std::to_string(ran->exit_code) +
                                     "): " + std::string(trimmed(ran->output)));
    }

    // For each object "<sha> <type> <size>\n<size bytes>\n", or "<sha> missing\n".
    std::ifstream in(file.path(), std::ios::binary);
    if (!in) return fail(ErrorKind::Io, "cannot read " + file.path().string());
    fetched.reserve(wanted.size());
    for (const auto& sha : wanted) {
      std::string header;
      if (!std::getline(in, header, '\n'))
        return fail(ErrorKind::Protocol, describe(site) + ": git cat-file output ends before blob " + sha);
      const std::vector<std::string_view> fields = split(header, ' ');
      if (fields.size() == 2 && fields[1] == "missing")
        return fail(ErrorKind::Io, describe(site) + ": blob " + sha + " is missing from the repository");
      if (fields.size() != 3 || fields[0] != sha)
        return fail(ErrorKind::Protocol, describe(site) + ": unexpected cat-file output: " + header.substr(0, 200));
      if (fields[1] != "blob") {
        return fail(ErrorKind::Protocol,
                    describe(site) + ": object " + sha + " is a " + std::string(fields[1]) + ", not a blob");
      }
      std::size_t size = 0;
      const auto parsed = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), size);
      if (parsed.ec != std::errc{} || parsed.ptr != fields[2].data() + fields[2].size())
        return fail(ErrorKind::Protocol, describe(site) + ": unexpected cat-file output: " + header.substr(0, 200));
      Blob item{sha, {}};  // not `blob`: that is a member function
      if (!resize_to(item.bytes, size)) {
        return fail(ErrorKind::Io, describe(site) + ": out of memory reading blob " + sha + " (" +
                                       std::to_string(size) + " bytes)");
      }
      in.read(item.bytes.data(), static_cast<std::streamsize>(size));
      if (static_cast<std::size_t>(in.gcount()) != size || in.get() != '\n')
        return fail(ErrorKind::Protocol, describe(site) + ": git cat-file output is cut short in blob " + sha);
      fetched.push_back(std::move(item));
    }
  }

  for (auto& item : fetched) {
    cached_bytes_ += item.bytes.size();
    lru_.push_front(std::move(item));
    index_.emplace(lru_.front().sha, lru_.begin());
  }
  // `asked` views the caller's strings; the cache's own keys are compared by value.
  for (auto it = lru_.end(); cached_bytes_ > config_.cache_bytes && it != lru_.begin();) {
    --it;
    if (asked.contains(it->sha)) continue;
    cached_bytes_ -= it->bytes.size();
    index_.erase(it->sha);
    it = lru_.erase(it);
  }
  return {};
}

Result<std::string_view> GitReader::blob(const std::string& blob_sha) {
  const auto cached = index_.find(blob_sha);
  if (cached == index_.end()) {
    return fail(ErrorKind::Io, "git repository " + config_.repo.string() + ": blob " + blob_sha +
                                   " is not in the cache (not fetched, or evicted since)");
  }
  lru_.splice(lru_.begin(), lru_, cached->second);
  return std::string_view(cached->second->bytes);
}

}  // namespace pychron::dvc
