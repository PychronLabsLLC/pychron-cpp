#include "pychron/experiment/persist/persister.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "pychron/experiment/record/serialize.hpp"

namespace pychron::experiment::persist {

namespace fs = std::filesystem;

namespace {

Unexpected<Error> io(const std::string& what) { return fail(ErrorKind::Io, what); }

bool safe_name(const std::string& s) {
  return !s.empty() && s.find('/') == std::string::npos && s.find('\\') == std::string::npos &&
         s.find("..") == std::string::npos;
}

// Write-then-rename so a crash never leaves a half-written file.
Result<void> write_atomic(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return io("cannot create " + path.parent_path().string() + ": " + ec.message());
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return io("cannot write " + tmp.string());
    out << text;
    out.flush();
    if (!out) return io("cannot write " + tmp.string());
  }
  fs::rename(tmp, path, ec);
  if (ec) return io("cannot rename " + tmp.string() + ": " + ec.message());
  return {};
}

Result<std::string> read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return io("cannot read " + path.string());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string run_name(const record::Identity& id) {
  return id.identifier + "-" + std::to_string(id.aliquot) + id.step;
}

}  // namespace

// ---- FilePersister ------------------------------------------------------------

int FilePersister::highest_on_disk(const std::string& identifier) const {
  int best = 0;
  std::error_code ec;
  const fs::path dir = root_ / identifier;
  if (!fs::is_directory(dir, ec)) return 0;
  const std::string prefix = identifier + "-";
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const auto name = e.path().filename().string();
    if (!name.starts_with(prefix) || !name.ends_with(".json")) continue;
    std::size_t i = prefix.size(), j = i;
    while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
    if (j == i) continue;
    best = std::max(best, std::stoi(name.substr(i, j - i)));
  }
  return best;
}

Result<int> FilePersister::next_aliquot(const std::string& identifier) {
  if (!safe_name(identifier)) return fail(ErrorKind::Config, "identifier '" + identifier + "' is not a plain name");
  std::lock_guard lock(mutex_);
  int& issued = issued_[identifier];
  issued = std::max(issued, highest_on_disk(identifier)) + 1;
  return issued;
}

Result<void> FilePersister::begin_run(const RunIdentity& id, const QueueSpec&) {
  if (!safe_name(id.identifier)) return fail(ErrorKind::Config, "identifier '" + id.identifier + "' is not a plain name");
  std::error_code ec;
  fs::create_directories(root_ / id.identifier, ec);
  if (ec) return io("cannot create " + (root_ / id.identifier).string() + ": " + ec.message());
  return {};
}

fs::path FilePersister::analysis_path(const record::AnalysisRecord& r) const {
  return root_ / r.identity.identifier / (run_name(r.identity) + ".json");
}

Result<void> FilePersister::save_extraction(const record::AnalysisRecord& r) {
  if (!safe_name(r.identity.identifier)) return fail(ErrorKind::Config, "bad identifier");
  return write_atomic(root_ / r.identity.identifier / (run_name(r.identity) + ".extraction.json"),
                      record::to_json(r));
}

Result<void> FilePersister::save_analysis(const record::AnalysisRecord& r) {
  if (!safe_name(r.identity.identifier)) return fail(ErrorKind::Config, "bad identifier");
  return write_atomic(analysis_path(r), record::to_json(r));
}

Result<void> FilePersister::save_artifact(const std::string& uuid, const std::string& name,
                                          const std::vector<std::uint8_t>& bytes) {
  if (!safe_name(uuid) || !safe_name(name)) return fail(ErrorKind::Config, "artifact names must be plain");
  return write_atomic(root_ / "artifacts" / uuid / name, std::string(bytes.begin(), bytes.end()));
}

// ---- Spool ----------------------------------------------------------------------

Result<void> Spool::put(const record::AnalysisRecord& r) {
  if (!safe_name(r.identity.uuid)) return fail(ErrorKind::Config, "record uuid missing or not plain");
  return write_atomic(dir_ / (r.identity.uuid + ".json"), record::to_json(r));
}

Result<void> Spool::remove(const std::string& uuid) {
  std::error_code ec;
  fs::remove(dir_ / (uuid + ".json"), ec);
  if (ec) return io("cannot remove spooled " + uuid + ": " + ec.message());
  return {};
}

Result<std::vector<std::string>> Spool::pending() const {
  std::error_code ec;
  if (!fs::is_directory(dir_, ec)) return std::vector<std::string>{};
  std::vector<std::pair<fs::file_time_type, std::string>> files;
  for (const auto& e : fs::directory_iterator(dir_, ec)) {
    if (e.path().extension() != ".json") continue;
    files.emplace_back(fs::last_write_time(e.path(), ec), e.path().stem().string());
  }
  if (ec) return io("cannot list " + dir_.string() + ": " + ec.message());
  std::sort(files.begin(), files.end());
  std::vector<std::string> out;
  for (auto& f : files) out.push_back(std::move(f.second));
  return out;
}

Result<record::AnalysisRecord> Spool::get(const std::string& uuid) const {
  auto text = read_text(dir_ / (uuid + ".json"));
  if (!text) return fail(text.error());
  return record::from_json(*text);
}

// ---- SavePipeline ---------------------------------------------------------------

Result<void> SavePipeline::hand_off(const std::string& uuid) {
  auto rec = spool_.get(uuid);
  Result<void> r = rec ? persister_.save_analysis(*rec) : Result<void>(fail(rec.error()));
  if (r) r = spool_.remove(uuid);
  std::lock_guard lock(mutex_);
  if (!r) last_error_ = uuid + ": " + r.error().what;
  return r;
}

Result<void> SavePipeline::save(const record::AnalysisRecord& record) {
  if (auto r = spool_.put(record); !r) return r;
  const std::string uuid = record.identity.uuid;
  if (post_) {
    post_([this, uuid] { (void)hand_off(uuid); });
  } else {
    (void)hand_off(uuid);  // a failure stays pending
  }
  return {};
}

Result<std::size_t> SavePipeline::flush() {
  auto uuids = spool_.pending();
  if (!uuids) return fail(uuids.error());
  std::size_t done = 0;
  std::optional<Error> first;
  for (const auto& u : *uuids) {
    if (auto r = hand_off(u); r) {
      ++done;
    } else if (!first) {
      first = r.error();
    }
  }
  if (auto r = persister_.flush(); !r && !first) first = r.error();
  if (done == 0 && first) return fail(*first);
  return done;
}

std::size_t SavePipeline::pending() const {
  auto p = spool_.pending();
  return p ? p->size() : 0;
}

std::optional<std::string> SavePipeline::last_error() const {
  std::lock_guard lock(mutex_);
  return last_error_;
}

// ---- AliquotAllocator -----------------------------------------------------------

Result<Allocation> AliquotAllocator::allocate(const RunIdentity& id) {
  if (id.aliquot) {
    if (*id.aliquot < 1) return fail(ErrorKind::Config, "aliquot must be >= 1, got " + std::to_string(*id.aliquot));
    return Allocation{*id.aliquot, id.step};
  }
  auto next = persister_.next_aliquot(id.identifier);
  if (!next) return fail(next.error());
  return Allocation{*next, id.step};
}

}  // namespace pychron::experiment::persist
