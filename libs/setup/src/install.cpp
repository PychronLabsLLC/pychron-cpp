#include "pychron/setup/install.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/systems/canvas/cross_validate.hpp"
#include "pychron/systems/canvas/loader.hpp"

namespace pychron::setup {

namespace fs = std::filesystem;

namespace {

Result<std::string> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Result<void> write_file(const fs::path& p, const std::string& content, bool secret) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + p.parent_path().string() + ": " + ec.message());
  if (secret) {
    // Created empty and restricted before the secret goes in.
    { std::ofstream create(p, std::ios::binary | std::ios::trunc); }
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
  }
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  if (!out) return fail(ErrorKind::Io, "cannot write " + p.string());
  return {};
}

// "@examples/plans/" -> <examples>/plans/; "{{ line_file }}" -> the file the
// user named; otherwise under the profile.
Result<fs::path> source_of(const FileSpec& f, const fs::path& examples, const Answers& answers) {
  constexpr std::string_view kExamples = "@examples/";
  if (std::string_view(f.copy).starts_with(kExamples)) return examples / f.copy.substr(kExamples.size());
  if (f.copy.find("{{") != std::string::npos) {
    auto named = render(f.copy, answers, f.profile + ": " + f.to);
    if (!named) return fail(std::move(named).error());
    if (named->empty()) return fail(ErrorKind::Config, f.to + ": no file given");
    return fs::path(*named);
  }
  return f.profile_dir / f.copy;
}

std::string first_lines(const std::string& text, int n) {
  std::string out;
  std::size_t at = 0;
  for (int i = 0; i < n && at < text.size(); ++i) {
    const auto nl = text.find('\n', at);
    out += (out.empty() ? "" : "\n") + text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  return out;
}

// Files marked check = "line" / "canvas" are loaded the way the programs
// load them, and a canvas is checked against the line it draws.
void check_files(const std::vector<PlannedFile>& files, const std::map<std::string, std::string>& checks,
                 std::vector<std::string>& errors) {
  std::optional<config::SystemConfig> line;
  for (const auto& f : files) {
    auto c = checks.find(f.to.generic_string());
    if (c == checks.end() || c->second != "line") continue;
    auto loaded = config::load_system_config_from_string(f.content, f.to.generic_string());
    if (loaded) line = std::move(*loaded);
    else errors.push_back(f.to.generic_string() + " is not a usable extraction-line config:\n" +
                          first_lines(loaded.error().what, 8));
  }
  for (const auto& f : files) {
    auto c = checks.find(f.to.generic_string());
    if (c == checks.end() || c->second != "canvas") continue;
    auto canvas = canvas::load_canvas_from_string(f.content, f.to.generic_string());
    if (!canvas) {
      errors.push_back(f.to.generic_string() + " is not a usable canvas:\n" + first_lines(canvas.error().what, 8));
      continue;
    }
    if (!line) continue;
    if (auto crossed = canvas::check_canvas(*canvas, *line); !crossed)
      errors.push_back(f.to.generic_string() + " does not match the extraction line:\n" +
                       first_lines(crossed.error().what, 8));
  }
}

bool is_secret_question(const ResolvedProfile& p, const std::string& id) {
  return std::any_of(p.questions.begin(), p.questions.end(),
                     [&](const Question& q) { return q.id == id && q.type == QuestionType::Secret; });
}

}  // namespace

std::string_view to_string(PlannedFile::Action a) noexcept {
  switch (a) {
    case PlannedFile::Action::Write: return "write";
    case PlannedFile::Action::Same: return "same";
    case PlannedFile::Action::Keep: return "keep";
    case PlannedFile::Action::Update: return "update";
    case PlannedFile::Action::Conflict: return "conflict";
  }
  return "?";
}

std::string sha256_hex(std::string_view bytes) {
  const auto digest = sha256(bytes);
  return to_hex(digest);
}

fs::path record_path(const fs::path& root) { return root / ".pychron" / "install.toml"; }

Result<InstallPlan> plan_install(const ProfileLibrary& library, const ResolvedProfile& profile, const Answers& answers,
                                 const fs::path& root, PlanOptions options) {
  InstallPlan plan;
  plan.root = root;
  plan.profile = profile;
  plan.answers = answers;
  std::optional<InstallRecord> record;
  if (options.reconfigure) {
    auto r = read_install_record(root);
    if (!r) return fail(ErrorKind::Config, "reconfigure: " + r.error().what);
    record = std::move(*r);
  }

  std::vector<std::string> errors;
  std::set<std::string> seen;
  std::map<std::string, std::string> checks;  // destination -> "line" / "canvas"
  auto add = [&](PlannedFile f) {
    const std::string key = f.to.generic_string();
    if (!seen.insert(key).second) {
      errors.push_back(key + ": written twice");
      return;
    }
    plan.files.push_back(std::move(f));
  };
  for (const auto& spec : profile.files) {
    if (spec.secret && options.skip_secret_files) continue;
    if (!spec.when.empty()) {
      auto c = evaluate(spec.when, answers);
      if (!c) {
        errors.push_back(spec.profile + ": " + spec.to + ": when \"" + spec.when + "\": " + c.error().what);
        continue;
      }
      if (!*c) continue;
    }
    if (!spec.template_path.empty()) {
      auto text = read_file(spec.profile_dir / spec.template_path);
      if (!text) {
        errors.push_back(text.error().what);
        continue;
      }
      auto rendered = render(*text, answers, spec.profile + "/" + spec.template_path);
      if (!rendered) {
        errors.push_back(rendered.error().what);
        continue;
      }
      if (!spec.check.empty()) checks[fs::path(spec.to).generic_string()] = spec.check;
      add(PlannedFile{spec.to, std::move(*rendered), spec.secret, spec.profile, PlannedFile::Action::Write});
      continue;
    }
    auto source = source_of(spec, library.examples(), answers);
    if (!source) {
      errors.push_back(source.error().what);
      continue;
    }
    const fs::path src = *source;
    if (!spec.check.empty()) checks[fs::path(spec.to).generic_string()] = spec.check;
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
      std::vector<fs::path> files;
      for (const auto& e : fs::recursive_directory_iterator(src, ec))
        if (e.is_regular_file()) files.push_back(e.path());
      std::sort(files.begin(), files.end());
      for (const auto& file : files) {
        const auto rel = fs::relative(file, src);
        const std::string name = rel.filename().string();
        if (name.starts_with(".") || name.ends_with("~") || name.ends_with(".state.toml")) continue;
        auto content = read_file(file);
        if (!content) {
          errors.push_back(content.error().what);
          continue;
        }
        add(PlannedFile{fs::path(spec.to) / rel, std::move(*content), spec.secret, spec.profile, PlannedFile::Action::Write});
      }
    } else {
      auto content = read_file(src);
      if (!content) {
        errors.push_back(spec.copy.find("{{") != std::string::npos
                             ? spec.to + ": cannot read " + src.string()
                             : spec.profile + ": " + content.error().what);
        continue;
      }
      add(PlannedFile{spec.to, std::move(*content), spec.secret, spec.profile, PlannedFile::Action::Write});
    }
  }

  check_files(plan.files, checks, errors);
  for (auto& f : plan.files) {
    // Every TOML file must parse before anything is written.
    if (f.to.extension() == ".toml") {
      auto parsed = toml::parse(f.content, f.to.generic_string());
      if (!parsed) errors.push_back(f.to.generic_string() + ": the rendered file does not parse: " +
                                    std::string(parsed.error().description()));
    }
    if (f.content.find("SIMULATION PLACEHOLDER") != std::string::npos || f.content.find("CONFIRM") != std::string::npos)
      plan.placeholders.push_back(f.to);
    const fs::path target = root / f.to;
    std::error_code ec;
    if (!fs::exists(target, ec)) continue;
    auto existing = read_file(target);
    if (!existing) {
      errors.push_back(existing.error().what);
      continue;
    }
    if (*existing == f.content) {
      f.action = PlannedFile::Action::Same;
    } else if (record) {
      auto it = record->files.find(f.to.generic_string());
      const bool untouched = it != record->files.end() && it->second == sha256_hex(*existing);
      f.action = untouched ? PlannedFile::Action::Update : PlannedFile::Action::Conflict;
    } else {
      f.action = PlannedFile::Action::Keep;
    }
  }
  if (!errors.empty()) {
    std::string all;
    for (const auto& e : errors) all += (all.empty() ? "" : "\n") + e;
    return fail(ErrorKind::Config, all);
  }
  return plan;
}

Result<InstallReport> apply_install(const InstallPlan& plan) {
  InstallReport report;
  std::error_code ec;
  fs::create_directories(plan.root, ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + plan.root.string() + ": " + ec.message());
  std::map<std::string, std::string> hashes;
  if (auto old = read_install_record(plan.root)) hashes = old->files;
  for (const auto& f : plan.files) {
    const fs::path target = plan.root / f.to;
    switch (f.action) {
      case PlannedFile::Action::Write:
      case PlannedFile::Action::Update:
        if (auto w = write_file(target, f.content, f.secret); !w) return fail(std::move(w).error());
        hashes[f.to.generic_string()] = sha256_hex(f.content);
        (f.action == PlannedFile::Action::Write ? report.written : report.updated).push_back(f.to);
        break;
      case PlannedFile::Action::Same:
        hashes[f.to.generic_string()] = sha256_hex(f.content);
        report.same.push_back(f.to);
        break;
      case PlannedFile::Action::Keep: report.kept.push_back(f.to); break;
      case PlannedFile::Action::Conflict: {
        fs::path fresh = f.to;
        fresh += ".new";
        if (auto w = write_file(plan.root / fresh, f.content, f.secret); !w) return fail(std::move(w).error());
        report.conflicts.push_back(fresh);
        break;
      }
    }
  }

  toml::table record;
  toml::table install;
  install.insert("profile", plan.profile.top.name);
  toml::table versions;
  for (const auto& [name, v] : plan.profile.versions) versions.insert(name, v);
  install.insert("versions", std::move(versions));
  record.insert("install", std::move(install));
  toml::table answers;
  for (const auto& [id, v] : plan.answers) {
    // Only answers to questions: built-ins and [values] come from the profile again.
    const bool question = std::any_of(plan.profile.questions.begin(), plan.profile.questions.end(),
                                      [&](const Question& q) { return q.id == id; });
    if (!question || is_secret_question(plan.profile, id)) continue;
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            toml::array a;
            for (const auto& s : x) a.push_back(s);
            answers.insert(id, std::move(a));
          } else if constexpr (std::is_same_v<T, std::vector<Row>>) {
            toml::array a;
            for (const auto& r : x) {
              toml::table row;
              for (const auto& [k, val] : r) row.insert(k, val);
              a.push_back(std::move(row));
            }
            answers.insert(id, std::move(a));
          } else {
            answers.insert(id, x);
          }
        },
        v);
  }
  record.insert("answers", std::move(answers));
  toml::table files;
  for (const auto& [path, hash] : hashes) files.insert(path, hash);
  record.insert("files", std::move(files));
  std::ostringstream out;
  out << "# Written by pychron setup. Do not edit: reconfigure and doctor read it.\n" << record << "\n";
  if (auto w = write_file(record_path(plan.root), out.str(), false); !w) return fail(std::move(w).error());
  return report;
}

Result<InstallRecord> read_install_record(const fs::path& root) {
  auto text = read_file(record_path(root));
  if (!text) return fail(std::move(text).error());
  auto parsed = toml::parse(*text, record_path(root).string());
  if (!parsed) return fail(ErrorKind::Config, record_path(root).string() + ": " + std::string(parsed.error().description()));
  const auto& t = parsed.table();
  InstallRecord r;
  r.profile = t["install"]["profile"].value_or(std::string{});
  if (const auto* v = t["install"]["versions"].as_table())
    for (const auto& [k, n] : *v) r.versions[std::string(k.str())] = n.value_or(std::int64_t{0});
  if (const auto* a = t["answers"].as_table()) {
    std::ostringstream ss;
    ss << *a;
    auto answers = answers_from_toml(ss.str(), "answers");
    if (!answers) return fail(std::move(answers).error());
    r.answers = std::move(*answers);
  }
  if (const auto* f = t["files"].as_table())
    for (const auto& [k, h] : *f) r.files[std::string(k.str())] = h.value_or(std::string{});
  if (r.profile.empty()) return fail(ErrorKind::Config, record_path(root).string() + ": no [install] profile");
  return r;
}

}  // namespace pychron::setup
